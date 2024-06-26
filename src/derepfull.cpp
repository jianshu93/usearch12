#include "myutils.h"
#include "seqdb.h"
#include "objmgr.h"
#include "seqinfo.h"
#include "sort.h"
#include "derep.h"
#include "alpha.h"
#include "fastq.h"
#include "seqhash.h"
#include "constaxstr.h"
#include "derep.h"
#include "progress.h"

bool StrandOptToRevComp(bool RequiredOpt, bool Default);

#define TRACE		0

static uint g_SeqCount;
static const byte * const *g_Seqs;
static bool g_Circles;
static bool g_RevComp;
const uint *g_SeqLengths;

void Thread(DerepThreadData *aTD)
	{
	uint SeqCount = g_SeqCount;
	const byte * const *Seqs = g_Seqs;
	const uint *SeqLengths = g_SeqLengths;
	bool Circles = g_Circles;
	bool RevComp = g_RevComp;

	DerepThreadData &TD = *aTD;
	const unsigned ThreadIndex = GetThreadIndex();
	//asserta(ThreadIndex < ThreadCount);

	//DerepThreadData &TD = TDs[ThreadIndex];
	const unsigned TDSeqCount = TD.SeqCount;
	unsigned &TDUniqueCount = TD.UniqueCount;

	unsigned SlotCount = UINT_MAX;
	if (ofilled(OPT_slots))
		SlotCount = oget_uns(OPT_slots);
	else
		{
		unsigned SlotCountLo = 8*TDSeqCount;
		unsigned SlotCountHi = 9*TDSeqCount;
		SlotCount = FindPrime(SlotCountLo, SlotCountHi);
		if (SlotCount == UINT_MAX)
			SlotCount = SlotCountLo;
		}

	if (SlotCount >= (unsigned) INT_MAX)
		Die("Too many slots");

	const uint32 *TDSeqIndexes = TD.SeqIndexes;
	const uint32 *TDSeqHashes = TD.SeqHashes;
	uint32 *TDHashTable = myalloc(uint32, SlotCount);
	uint32 *TDClusterSIs = myalloc(uint32, SeqCount);
	uint32 *TDUniqueSeqIndexes = myalloc(uint32, SeqCount);
	bool *TDStrands = myalloc(bool, SeqCount);

	TD.ClusterSIs = TDClusterSIs;
	TD.UniqueSeqIndexes = TDUniqueSeqIndexes;
	TD.Strands = TDStrands;

	for (unsigned i = 0; i < SlotCount; ++i)
		TDHashTable[i] = UINT_MAX;

	for (unsigned i = 0; i < TDSeqCount; ++i)
		{
		unsigned SeqIndex = TDSeqIndexes[i];
		assert(SeqIndex < SeqCount);

		unsigned Count = 0;
		unsigned QuerySeqIndex = TDSeqIndexes[i];
		const unsigned QueryHash = TDSeqHashes[i];
		unsigned h = QueryHash%SlotCount;
		for (;;)
			{
			unsigned UniqueSeqIndex = TDHashTable[h];
			if (UniqueSeqIndex == UINT_MAX)
				{
				TDHashTable[h] = QuerySeqIndex;
				TDClusterSIs[i] = QuerySeqIndex;
				TDStrands[i] = true;
				TDUniqueSeqIndexes[TDUniqueCount++] = QuerySeqIndex;
				break;
				}

			assert(UniqueSeqIndex < SeqCount);
			bool Eq = false;
			bool RCEq = false;
			unsigned QL = SeqLengths[QuerySeqIndex];
			unsigned UL = SeqLengths[UniqueSeqIndex];
			if (QL == UL)
				{
				const byte *Q = Seqs[QuerySeqIndex];
				const byte *U = Seqs[UniqueSeqIndex];
				Eq = false;
				if (Circles)
					{
					asserta(false);
					//Eq = SeqEqCircle(Q, QL, U, UL);
					}
				else
					{
					Eq = SeqEq(Q, QL, U, UL);
					if (RevComp)
						{
						RCEq = SeqEqRC(Q, QL, U, UL);
						Eq = (Eq || RCEq);
						}
					}
				}
			if (Eq)
				{
				TDClusterSIs[i] = UniqueSeqIndex;
				TDStrands[i] = !RCEq;
				break;
				}
			h = (h+1)%SlotCount;
			asserta(Count++ < SlotCount);
			}
		}

	//TDs[ThreadIndex].Done = true;
	TD.Done = true;
	}

void DerepFull(const SeqDB &Input, DerepResult &DR, bool RevComp, bool Circles)
	{
	asserta(!Circles);
	unsigned ThreadCount = GetRequestedThreadCount();
	DR.m_Input = &Input;

	unsigned SeqCount = Input.GetSeqCount();
	if (SeqCount > INT_MAX)
		Die("Too many seqs");

	DerepThreadData *TDs = myalloc(DerepThreadData, ThreadCount);
	for (unsigned ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
		{
		DerepThreadData &TD = TDs[ThreadIndex];
		TD.SeqIndexes = myalloc(uint32, SeqCount);
		TD.SeqHashes = myalloc(uint32, SeqCount);
		TD.SeqCount = 0;
		TD.ClusterSIs = 0;
		TD.Strands = 0;
		TD.UniqueSeqIndexes = 0;
		TD.UniqueCount = 0;
		TD.Done = false;
		}

	const byte * const *Seqs = Input.m_Seqs;
	g_Seqs = Seqs;
	const unsigned *SeqLengths = Input.m_SeqLengths;

	unsigned TooShortCount = 0;
	for (unsigned SeqIndex = 0; SeqIndex < SeqCount; ++SeqIndex)
		{
		const byte *Seq = Seqs[SeqIndex];
		unsigned L = SeqLengths[SeqIndex];

		uint32 SeqHash = UINT32_MAX;
		if (Circles)
			{
			asserta(false);
			//SeqHash = SeqHashCircle(Seq, L);
			}
		else
			{
			SeqHash = SeqHash32(Seq, L);
			if (RevComp)
				SeqHash = min(SeqHash, SeqHashRC32(Seq, L));
			}

		unsigned T = SeqHash%ThreadCount;
		DerepThreadData &TD = TDs[T];
		unsigned k = TD.SeqCount++;
		TD.SeqIndexes[k] = SeqIndex;
		TD.SeqHashes[k] = SeqHash;
		}

	unsigned FPs = 0;

	g_SeqCount = SeqCount;
	g_Seqs = Seqs;
	g_SeqLengths = SeqLengths;
	g_Circles = Circles;
	g_RevComp = RevComp;

	vector<thread *> ts;
	ProgressStartOther("Unique seqs.");
	for (uint ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
		{
		DerepThreadData *TD = &TDs[ThreadIndex];
		thread *t = new thread(Thread, TD);
		ts.push_back(t);
		}
	for (uint ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
		ts[ThreadIndex]->join();
	ProgressDoneOther();

	DR.FromThreadData(TDs, ThreadCount, true, 1);

	for (unsigned ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
		{
		DerepThreadData &TD = TDs[ThreadIndex];
		asserta(TD.Done);
		TD.Free();
		}
	}

static void Derep(const string &FileName)
	{
	if (ofilled(OPT_output))
		Die("Use -fastaout, not -output");

	bool RevComp = StrandOptToRevComp(false, false);
	bool Circles = false;

	SeqDB Input;
	Input.FromFastx(FileName);
	if (Circles && !Input.GetIsNucleo())
		Die("-circles not allowed with amino acid input");

	DerepResult DR;
	DerepFull(Input, DR, RevComp, Circles);

	DR.Write();
	}

void cmd_fastx_uniques()
	{
	Derep(oget_str(OPT_fastx_uniques));
	}
