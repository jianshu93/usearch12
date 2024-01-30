#include "myutils.h"
#include "seqinfo.h"
#include "searcher.h"
#include "seqsource.h"
#include "objmgr.h"
#include "hitmgr.h"
#include "seqdb.h"
#include "udbusortedsearcher.h"
//#include "phixfinder.h"
#include "globalaligner.h"
#include "alignresult.h"
#include "label.h"
#include "annotator.h"
#include "fastq.h"
#include <math.h>

void InitGlobals(bool Nucleo);
void Uchime2DeNovo(const SeqDB &Input, vector<bool> &IsChimeraVec, vector<string> &InfoStrs);

static FILE *g_fTab;

static bool IsAccept(AlignResult *AR)
	{
	if (AR == 0)
		return false;

	unsigned DiffsQT = AR->GetMismatchCount();
	if (DiffsQT == 0)
		return true;

	unsigned QSize = AR->m_Query->GetSize();
	unsigned TSize = AR->m_Target->GetSize();

/***
unoise_function.py

def f(d, x):
	return 2**((d*x) + 1)


  d    x=  1.0        1.5        2.0        2.5        3.0
  1        4.0        5.7        8.0       11.3       16.0
  2        8.0       16.0       32.0       64.0      128.0
  3       16.0       45.3      128.0      362.0     1024.0
  4       32.0      128.0      512.0     2048.0     8192.0
  5       64.0      362.0     2048.0    11585.2    65536.0
  6      128.0     1024.0     8192.0    65536.0   524288.0
  7      256.0     2896.3    32768.0   370727.6  4194304.0
***/

	double Skew = double(TSize)/double(QSize);

	const double Alpha = opt(unoise_alpha);
	double v = DiffsQT*Alpha + 1.0;
	double MinSkew = pow(2.0, v);
	bool Accept = (Skew >= MinSkew);
	return Accept;
	}

static unsigned *g_TargetIndexes;
static unsigned *g_TotalSizes;

static unsigned SearchDenoise(SeqInfo *Query, UDBUsortedSearcher *USS, unsigned *ptrDiffs)
	{
	*ptrDiffs = UINT_MAX;
	unsigned HotCount = USS->GetHot(Query, MAX_HOT, MAX_DROP, g_TargetIndexes);
	if (HotCount == 0)
		return UINT_MAX;

	GlobalAligner *GA = (GlobalAligner *) USS->m_Aligner;
	GA->SetQuery(Query);
	unsigned BestTargetIndex = UINT_MAX;
	unsigned BestDiffs = UINT_MAX;
	unsigned AcceptCount = 0;
	unsigned MaxAccepts = opt(maxaccepts);
	for (unsigned HotIndex = 0; HotIndex < HotCount; ++HotIndex)
		{
		unsigned TargetIndex = g_TargetIndexes[HotIndex];
		SeqInfo *Target = ObjMgr::GetSeqInfo();
		USS->GetTargetSeqInfo(TargetIndex, Target);
		GA->SetTarget(Target);
		AlignResult *AR = GA->Align();
		if (AR != 0)
			{
			bool Accept = IsAccept(AR);
			if (Accept)
				{
				++AcceptCount;
				unsigned Diffs = AR->GetMismatchCount();
				if (Diffs < BestDiffs)
					{
					BestTargetIndex = TargetIndex;
					BestDiffs = Diffs;
					*ptrDiffs = Diffs;
					}
				ObjMgr::Down(AR);
				}
			}
		GA->OnTargetDone(Target);
		ObjMgr::Down(Target);
		if (BestDiffs <= 1)
			break;
		if (AcceptCount >= MaxAccepts)
			break;
		}
	GA->OnQueryDone(Query);
	return BestTargetIndex;
	}

void cmd_unoise3()
	{
	string InputFileName = opt(unoise3);

	if (optset_fastaout)
		Die("-fastaout not supported, use -zotus");

	if (!optset_abskew)
		{
		optset_abskew = true;
		opt_abskew = 16.0;
		}

	if (optset_tabbedout)
		g_fTab = CreateStdioFile(opt(tabbedout));

	InitGlobals(true);

	// PhixFinder::GlobalInit();

	SeqDB Input;
	Input.FromFastx(InputFileName);
	const unsigned InputSeqCount = Input.GetSeqCount();

	g_TargetIndexes = myalloc(unsigned, InputSeqCount);
	g_TotalSizes = myalloc(unsigned, InputSeqCount);

	GlobalAligner *GA = new GlobalAligner;
	GA->Init();
	GA->m_FailIfNoHSPs = true;
	GA->m_FullDPAlways = false;

	UDBUsortedSearcher *USS = new UDBUsortedSearcher;
	UDBParams Params;
	Params.FromCmdLine(CMD_unoise3, true);
	USS->CreateEmpty(Params);
	USS->InitSearcher(0, GA, 0, 0);
	USS->m_MinFractId = 0.9;

	unsigned MinAmpSize = 8;
	if (optset_minsize)
		MinAmpSize = opt(minsize);
	unsigned MinQSize = MinAmpSize;
	unsigned UniqCount = InputSeqCount;
	for (unsigned SeqIndex = 0; SeqIndex < InputSeqCount; ++SeqIndex)
		{
		SeqInfo *Query = ObjMgr::GetSeqInfo();
		Input.GetSI(SeqIndex, *Query);
		unsigned QSize = Query->GetSize();
		if (QSize < MinAmpSize)
			{
			UniqCount = SeqIndex;
			break;
			}
		}

	unsigned GoodCount = 0;
	unsigned CorrectedCount = 0;
	vector<unsigned> UniqIndexToAmpIndex;
	vector<unsigned> UniqIndexToDiffs;
	for (unsigned SeqIndex = 0; SeqIndex < UniqCount; ++SeqIndex)
		{
		SeqInfo *Query = ObjMgr::GetSeqInfo();
		Input.GetSI(SeqIndex, *Query);
		unsigned QSize = Query->GetSize();
		asserta(QSize >= MinAmpSize);
		ProgressStep(SeqIndex, UniqCount, "%u amplicons, %u bad (size >= %u)",
		  GoodCount, CorrectedCount, QSize);
		unsigned Diffs = UINT_MAX;
		unsigned TargetIndex = SearchDenoise(Query, USS, &Diffs);
		if (TargetIndex != UINT_MAX)
			{
			CorrectedCount += QSize;
			g_TotalSizes[TargetIndex] += QSize;

			if (g_fTab != 0)
				{
				const char *TargetLabel = USS->GetTargetLabel(TargetIndex);

				string TopAcc;
				GetAccFromLabel(TargetLabel, TopAcc);

				fprintf(g_fTab, "%s\tdenoise", Query->m_Label);
				if (Diffs == 0)
					{
					static bool WarningDone = false;
					if (!WarningDone)
						{
						Warning("Shifted sequences detected");
						WarningDone = true;
						}
					fprintf(g_fTab, "\tshifted");
					}
				else
					fprintf(g_fTab, "\tbad");
				fprintf(g_fTab, "\tdqt=%u;top=%s;", Diffs, TopAcc.c_str());
				fprintf(g_fTab, "\n");
				}
			}
		else
			{
			++GoodCount;
			TargetIndex = USS->AddSIToDB_CopyData(Query);
			Diffs = 0;
			g_TotalSizes[TargetIndex] = QSize;

			if (g_fTab != 0)
				{
				fprintf(g_fTab, "%s\tdenoise", Query->m_Label);
				fprintf(g_fTab, "\tamp%u", TargetIndex + 1);
				fprintf(g_fTab, "\n");
				}
			}
		UniqIndexToAmpIndex.push_back(TargetIndex);
		UniqIndexToDiffs.push_back(Diffs);
		ObjMgr::Down(Query);
		}
	asserta(SIZE(UniqIndexToAmpIndex) == UniqCount);

	SeqDB AmpDB;
	const SeqDB &DB = *USS->m_SeqDB;
	unsigned DBSeqCount = DB.GetSeqCount();
	asserta(DBSeqCount == GoodCount);
	const unsigned AmpCount = DBSeqCount;
	unsigned LastSize = UINT_MAX;
	for (unsigned AmpIndex = 0; AmpIndex < AmpCount; ++AmpIndex)
		{
		string Label = DB.GetLabel(AmpIndex);
		unsigned Size = GetSizeFromLabel(Label, UINT_MAX);
		// unsigned TotalSize = g_TotalSizes[AmpIndex];
		asserta(Size <= LastSize);
		LastSize = Size;

		string Acc;
		GetAccFromLabel(Label, Acc);

		string NewLabel;
		Ps(NewLabel, "Amp%u;uniq=%s;size=%u;", AmpIndex+1, Acc.c_str(), Size);

		const byte *Seq = DB.GetSeq(AmpIndex);
		const unsigned L = DB.GetSeqLength(AmpIndex);
		const char *SavedLabel = mystrsave(NewLabel.c_str());
		AmpDB.AddSeq_CopyPtrs(SavedLabel, Seq, L);
		}

	vector<bool> IsChimeraVec;
	vector<string> InfoStrs;
	Uchime2DeNovo(AmpDB, IsChimeraVec, InfoStrs);
	asserta(SIZE(IsChimeraVec) == DBSeqCount);

	FILE *fAmp = 0;
	if (optset_ampout)
		fAmp = CreateStdioFile(opt(ampout));
	vector<unsigned> AmpIndexToOTUIndex;
	unsigned OTUCount = 0;
	unsigned ChimeraCount = 0;
	for (unsigned AmpIndex = 0; AmpIndex < DBSeqCount; ++AmpIndex)
		{
		string AmpType;
		string InfoStr = InfoStrs[AmpIndex];
		bool IsChimera = IsChimeraVec[AmpIndex];
		if (IsChimera)
			{
			AmpIndexToOTUIndex.push_back(UINT_MAX);
			++ChimeraCount;
			AmpType = "amptype=chimera;" + InfoStr;
			}
		else
			{
			AmpIndexToOTUIndex.push_back(OTUCount);
			++OTUCount;
			AmpType = "amptype=otu;";
			}

		const byte *Seq = DB.GetSeq(AmpIndex);
		const unsigned L = DB.GetSeqLength(AmpIndex);
		string Label = string(DB.GetLabel(AmpIndex));
		string NewLabel = Label + AmpType;
		SeqToFasta(fAmp, Seq, L, NewLabel.c_str());
		if (g_fTab != 0)
			{
			fprintf(g_fTab, "%s\tchfilter", Label.c_str());
			if (IsChimera)
				fprintf(g_fTab, "\tchimera\t%s", InfoStr.c_str());
			else
				fprintf(g_fTab, "\tzotu");
			fprintf(g_fTab, "\n");
			}
		}
	CloseStdioFile(fAmp);
	fAmp = 0;

	if (optset_zotus)
		{
		FILE *f = CreateStdioFile(opt(zotus));
		for (unsigned AmpIndex = 0; AmpIndex < AmpCount; ++AmpIndex)
			{
			ProgressStep(AmpIndex, AmpCount, "Writing zotus");
			if (IsChimeraVec[AmpIndex])
				continue;

			//const char *Label = AmpDB.GetLabel(AmpIndex);
			const byte *Seq = AmpDB.GetSeq(AmpIndex);
			unsigned L = AmpDB.GetSeqLength(AmpIndex);

			//const char *sc = strchr(Label, ';');
			//if (sc == 0)
			//	sc = Label;
			//else
			//	++sc;

			unsigned OTUIndex = AmpIndexToOTUIndex[AmpIndex];
			string NewLabel;
			//Ps(NewLabel, "Zotu%u;%s", OTUIndex + 1, sc);
			Ps(NewLabel, "Zotu%u", OTUIndex + 1);

			SeqToFasta(f, Seq, L, NewLabel.c_str());
			}
		CloseStdioFile(f);
		}
	CloseStdioFile(g_fTab);
	}