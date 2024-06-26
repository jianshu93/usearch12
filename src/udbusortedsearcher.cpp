#include "myutils.h"
#include "udbusortedsearcher.h"
#include "gobuff.h"
#include "seqdb.h"
#include "xdpmem.h"
#include "objmgr.h"
#include "seqinfo.h"
#include "hspfinder.h"
#include "alignresult.h"
#include "sort.h"

unsigned GetMinWindexWordCount(unsigned QueryUniqueWordCount, double FractId,
  unsigned WordLength, bool Nucleo);

bool UDBUsortedSearcher::HasSeqDB() const
	{
	return true;
	}

SeqDB *UDBUsortedSearcher::GetSeqDB() const
	{
	return m_UDBData->m_SeqDB;
	}

UDBUsortedSearcher::UDBUsortedSearcher(UDBData *data) : UDBSearcher(data)
	{
	m_MinFractId = 0.0f;
	m_Big = false;
	m_Self = false;
	}

UDBUsortedSearcher::UDBUsortedSearcher()
	{
	m_MinFractId = 0.0f;
	m_Big = false;
	m_Self = false;
	}

void UDBUsortedSearcher::SetQueryImpl()
	{
	if (!m_Big)
		{
		unsigned SeqCount = GetSeqCount();
		m_Big = (SeqCount > oget_uns(OPT_big));
		uint32 *U = m_U.Data;
		if (m_Big)
			{
			// ProgressLog("Big\n");
			for (unsigned i = 0; i < m_U.MaxSize; ++i)
				U[i] = 0;
#if	TIMING
			Log("\n");
			Log("BIG:\n");
			LogTiming();
#endif
			}
		}
	}

void UDBUsortedSearcher::SetTargetImpl()
	{
// Empty
	}

void UDBUsortedSearcher::OnQueryDoneImpl()
	{
	if (m_Big)
		{
	// Zero out previous
		unsigned N = m_TopTargetIndexes.Size;
		uint32 *TopTargetIndexes = m_TopTargetIndexes.Data;
		uint32 *U = m_U.Data;
		//Log("Zero prev\n");
		//LogVecs();
		for (unsigned i = 0; i < N; ++i)
			{
			uint32 TargetIndex = TopTargetIndexes[i];
			assert(TargetIndex < m_U.Size);
			assert(U[TargetIndex] > 0);
			U[TargetIndex] = 0;
			}
		m_TopTargetIndexes.Size = 0;
		}
	}

void UDBUsortedSearcher::OnTargetDoneImpl()
	{
// Empty
	}

void UDBUsortedSearcher::UDBSearchInit()
	{
	if (m_UDBData->m_Params.DBIsCoded())
		Die(".udb not supported by this command");

	asserta(m_UDBData->m_Params.m_StepPrefix == 0);

	if (!ofilled(OPT_id))
		Die("--id not set");

	m_MinFractId = float(oget_flt(OPT_id));
	if (m_MinFractId > 1.0)
		Die("-id out of range, should be 0.0 to 1.0");

	m_Self = oget_flag(OPT_self);
	}

// Input: m_Query, Output: m_TopOrder, m_TopTargetIndexes
void UDBUsortedSearcher::SetTargetOrder()
	{
	SetQueryWordsAllNoBad();
	SetQueryUniqueWords();

	const uint MinU = 1;
	const uint QueryStep = 1;

	SetU(QueryStep);
	SetTop(MinU);
	SortTop();
	}

void UDBUsortedSearcher::SearchImpl()
	{
	if (m_Big)
		{
		UDBSearchBig();
		return;
		}

	SetTargetOrder();
	const unsigned TopCount = m_TopOrder.Size;
	if (TopCount == 0)
		return;

	const unsigned *TopOrder = m_TopOrder.Data;
	const unsigned *TopTargetIndexes = m_TopTargetIndexes.Data;
	ObjMgr *OM = m_Query->m_Owner;
	for (unsigned k = 0; k < TopCount; ++k)
		{
		unsigned i = TopOrder[k];
		unsigned TargetIndex = TopTargetIndexes[i];

		m_Target = OM->GetSeqInfo();
		GetTargetSeqInfo(TargetIndex, m_Target);
		SetTarget(m_Target);

		bool Terminate = Align();
		m_Target->Down();
		if (Terminate)
			return;
		}
	}

void UDBUsortedSearcher::CountSortTop()
	{
	const unsigned N = m_TopU.Size;
	m_TopOrder.Alloc(N);

	unsigned K =
	  CountSortOrderDesc(m_TopU.Data, N, m_CSMem, m_TopOrder.Data);
	m_TopOrder.Size = K;
	}

void UDBUsortedSearcher::QuickSortTop()
	{
	const unsigned N = m_TopU.Size;
	m_TopOrder.Alloc(N);

	QuickSortOrderDesc(m_TopU.Data, N, m_TopOrder.Data);
	m_TopOrder.Size = N;
	}

void UDBUsortedSearcher::AlignAll()
	{
	const unsigned SeqCount = GetSeqCount();
	ObjMgr *OM = m_Query->m_Owner;
	for (unsigned TargetIndex = 0; TargetIndex < SeqCount; ++TargetIndex)
		{
		m_Target = OM->GetSeqInfo();
		m_UDBData->m_SeqDB->GetSI(TargetIndex, *m_Target);
		bool Ok = SetTarget(m_Target);
		if (Ok)
			Align();
		m_Target->Down();

	// Hack to keep terminator happy
		m_Terminator->m_AcceptCount = 0;
		m_Terminator->m_RejectCount = 0;
		}
	}

void UDBUsortedSearcher::SortTop()
	{
	if (oget_flag(OPT_quicksort))
		QuickSortTop();
	else
		CountSortTop();
	}

unsigned UDBUsortedSearcher::GetSeqCount() const
	{
	return m_UDBData->m_SeqDB->GetSeqCount();
	}

void UDBUsortedSearcher::SetTopNoBump(unsigned MinU)
	{
	const unsigned SeqCount = GetSeqCount();
	const unsigned *U = m_U.Data;
	asserta(m_U.Size == SeqCount);

	unsigned *TopU = m_TopU.Data;
	unsigned *TopTargetIndexes = m_TopTargetIndexes.Data;
	unsigned TopCount = 0;

	for (unsigned TargetIndex = 0; TargetIndex < SeqCount; ++TargetIndex)
		{
		unsigned n = U[TargetIndex];
		if (n >= MinU)
			{
			TopU[TopCount] = n;
			TopTargetIndexes[TopCount] = TargetIndex;
			++TopCount;
			}
		}

	m_TopU.Size = TopCount;
	m_TopTargetIndexes.Size = TopCount;
	}

void UDBUsortedSearcher::SetTopBump(unsigned MinU, unsigned BumpPct)
	{
	double Bump = BumpPct/100.0;
	unsigned SavedMinU = MinU;
	const unsigned *U = m_U.Data;

	const unsigned SeqCount = GetSeqCount();
//	asserta(m_U.Size == SeqCount);
	if (m_U.Size != SeqCount)
		Die("UDBUSortedSearcher::SetTopBump(), m_U.Size=%u, GetSeqCount()=%u",
		  m_U.Size, SeqCount);

	unsigned *TopU = m_TopU.Data;
	unsigned *TopTargetIndexes = m_TopTargetIndexes.Data;
	unsigned TopCount = 0;
	unsigned MaxCount = 0;
	unsigned MaxU = 0;
	for (unsigned TargetIndex = 0; TargetIndex < SeqCount; ++TargetIndex)
		{
		unsigned n = U[TargetIndex];
		if (n >= MinU)
			{
			if (n > MaxCount)
				{
				unsigned NewMinCount = unsigned(n*Bump);
				if (NewMinCount > MinU && NewMinCount < MaxCount)
					MinU = NewMinCount;
				MaxCount = n;
				}
			TopU[TopCount] = n;
			TopTargetIndexes[TopCount] = TargetIndex;
			++TopCount;
			}
		}

	m_TopU.Size = TopCount;
	m_TopTargetIndexes.Size = TopCount;
	}

void UDBUsortedSearcher::SetTop(unsigned MinU)
	{
	const unsigned SeqCount = GetSeqCount();
	m_TopU.Alloc(SeqCount);
	m_TopTargetIndexes.Alloc(SeqCount);

	if (MinU == 0)
		MinU = 1;

	if (oget_uns(OPT_bump) != 0)
		SetTopBump(MinU, oget_uns(OPT_bump));
	else
		SetTopNoBump(MinU);
	}

void UDBUsortedSearcher::SetU(unsigned QueryStep)
	{
	if (m_UDBData->m_Params.DBIsCoded())
		SetU_Coded(QueryStep);
	else
		SetU_NonCoded(QueryStep);
	}

void UDBUsortedSearcher::SetU_Coded(unsigned QueryStep)
	{
	const unsigned SeqCount = GetSeqCount();
	const uint32 *Sizes = m_UDBData->m_Sizes;
	const uint32 * const *UDBRows = m_UDBData->m_UDBRows;

	const unsigned QueryUniqueWordCount = m_QueryUniqueWords.Size;
	const uint32 *QueryUniqueWords = m_QueryUniqueWords.Data;

	assert(!m_Big);
	m_U.Alloc(SeqCount);
	m_U.Size = SeqCount;
	if (SeqCount == 0)
		return;

	unsigned *U = m_U.Data;
	zero_array(U, SeqCount);
	asserta(m_UDBData->m_Params.m_StepPrefix == 0);

	for (unsigned i = 0; i < QueryUniqueWordCount; i += QueryStep)
		{
		uint32 Word = QueryUniqueWords[i];
		assert(Word < m_UDBData->m_SlotCount);

		const uint32 *Row = UDBRows[Word];
		const unsigned Size = Sizes[Word];

		for (unsigned j = 0; j < Size; ++j)
			{
			unsigned Code = Row[j];

			unsigned TargetIndex;
			unsigned TargetPos;
			m_UDBData->m_Params.DecodeSeqPos(Code, TargetIndex, TargetPos);
			++(U[TargetIndex]);
			}
		}
	}

void UDBUsortedSearcher::SetU_VarCoded(unsigned QueryStep)
	{
	const unsigned SeqCount = GetSeqCount();
	const uint32 *Sizes = m_UDBData->m_Sizes;
	const uint32 * const *UDBRows = m_UDBData->m_UDBRows;

	const unsigned QueryUniqueWordCount = m_QueryUniqueWords.Size;
	const uint32 *QueryUniqueWords = m_QueryUniqueWords.Data;

	assert(!m_Big);
	m_U.Alloc(SeqCount);
	m_U.Size = SeqCount;
	if (SeqCount == 0)
		return;

	unsigned *U = m_U.Data;
	zero_array(U, SeqCount);
	asserta(m_UDBData->m_Params.m_StepPrefix == 0);

	for (unsigned i = 0; i < QueryUniqueWordCount; i += QueryStep)
		{
		uint32 Word = QueryUniqueWords[i];
		assert(Word < m_UDBData->m_SlotCount);

		const byte *Row = (const byte *) UDBRows[Word];
		const unsigned Size = Sizes[Word];

		unsigned Pos = 0;
		for (;;)
			{

			if (Pos >= Size)
				break;

			unsigned k;
			unsigned TargetIndex = DecodeUint32Var(Row + Pos, k);
			Pos += k;
			unsigned TargetPos = DecodeUint32Var(Row + Pos, k);
			Pos += k;
			++(U[TargetIndex]);
			}
		}
	}

void UDBUsortedSearcher::SetU_NonCoded(unsigned QueryStep)
	{
	const unsigned SeqCount = GetSeqCount();
	const uint32 *Sizes = m_UDBData->m_Sizes;
	const uint32 * const *UDBRows = m_UDBData->m_UDBRows;

	const unsigned QueryUniqueWordCount = m_QueryUniqueWords.Size;
	const uint32 *QueryUniqueWords = m_QueryUniqueWords.Data;

//	assert(!m_Big); // used by utax
	m_U.Alloc(SeqCount);
	m_U.Size = SeqCount;
	if (SeqCount == 0)
		return;

	unsigned *U = m_U.Data;
	zero_array(U, SeqCount);

	asserta(m_UDBData->m_Params.m_StepPrefix == 0);


	for (unsigned i = 0; i < QueryUniqueWordCount; i += QueryStep)
		{
		uint32 Word = QueryUniqueWords[i];
		assert(Word < m_UDBData->m_SlotCount);

		const uint32 *Row = UDBRows[Word];
		const unsigned Size = Sizes[Word];

		for (unsigned j = 0; j < Size; ++j)
			{
			unsigned TargetIndex = Row[j];
			++(U[TargetIndex]);
			}
		}
	}

void UDBUsortedSearcher::GetWordCountingParams(float MinFractId,
  unsigned QueryUniqueWordCount, unsigned &MinU, unsigned &Step)
	{
	asserta(m_UDBData->m_Params.m_StepPrefix == 0);

	void GetWordCountingParams(float MinFractId, unsigned QueryUniqueWordCount,
	  unsigned DBStep, unsigned WordOnes, bool IsNucleo,
	  unsigned &MinU, unsigned &Step);

	GetWordCountingParams(MinFractId, QueryUniqueWordCount, m_UDBData->m_Params.m_DBStep,
	  m_UDBData->m_Params.m_WordOnes, m_UDBData->m_Params.m_IsNucleo, MinU, Step);
	}

void UDBUsortedSearcher::GetTargetSeqInfo(unsigned TargetIndex, SeqInfo *SI)
	{
	m_UDBData->m_SeqDB->GetSI(TargetIndex, *SI);
	}

unsigned UDBUsortedSearcher::GetTopTargetIndex(SeqInfo *Query, unsigned *ptrU, unsigned *ptrN)
	{
	asserta(!m_UDBData->m_Params.DBIsCoded());

	const unsigned SeqCount = GetSeqCount();
	m_TopU.Alloc(SeqCount);
	m_TopTargetIndexes.Alloc(SeqCount);

	m_Query = Query;
	SetQueryImpl();
	SetQueryWordsAllNoBad();
	SetQueryUniqueWords();
	SetU_NonCoded(1);

	*ptrN = m_QueryUniqueWords.Size;
	*ptrU = 0;
	unsigned TopTargetIndex = UINT_MAX;
	const unsigned *U = m_U.Data;
	for (unsigned TargetIndex = 0; TargetIndex < SeqCount; ++TargetIndex)
		{
		unsigned u = U[TargetIndex];
		if (u > *ptrU)
			{
			*ptrU = u;
			TopTargetIndex = TargetIndex;
			}
		}
	return TopTargetIndex;
	}

unsigned UDBUsortedSearcher::DeleteSelf(SeqInfo *Query, unsigned *TargetIndexes,
  unsigned *WordCounts, unsigned N)
	{
	if (N == 0)
		return 0;

	SeqDB &DB = *m_UDBData->m_SeqDB;
	const char *QueryLabel = Query->m_Label;
	unsigned TopWordCount = WordCounts[0];
	for (unsigned i = 0; i < N; ++i)
		{
		unsigned WordCount = WordCounts[i];
		if (WordCount < TopWordCount)
			break;
		unsigned TargetIndex = TargetIndexes[i];
		const char *TargetLabel = DB.GetLabel(TargetIndex);
		if (strcmp(QueryLabel, TargetLabel) == 0)
			{
			for (unsigned j = i; j + 1 < N; ++j)
				{
				TargetIndexes[j] = TargetIndexes[j+1];
				WordCounts[j] = WordCounts[j+1];
				}
			return N-1;
			}
		}
	return N;
	}

unsigned UDBUsortedSearcher::GetU(SeqInfo *Query, unsigned *TargetIndexes, unsigned *WordCounts)
	{
	asserta(!m_UDBData->m_Params.DBIsCoded());

	const unsigned SeqCount = GetSeqCount();
	m_TopU.Alloc(SeqCount);
	m_TopTargetIndexes.Alloc(SeqCount);

	m_Query = Query;
	SetQueryImpl();
	SetQueryWordsAllNoBad();
	SetQueryUniqueWords();
	if (m_UDBData->m_Params.DBIsVarCoded())
		SetU_VarCoded(1);
	else if (m_UDBData->m_Params.DBIsCoded())
		SetU_Coded(1);
	else
		SetU_NonCoded(1);
	SetTopNoBump(1);
	SortTop();

	unsigned N = m_TopOrder.Size;
	const unsigned *U = m_U.Data;
	const unsigned *TopTargetIndexes = m_TopTargetIndexes.Data;
	const unsigned *TopOrder = m_TopOrder.Data;
	unsigned LastWordCount = UINT_MAX;
	for (unsigned i = 0; i < N; ++i)
		{
		unsigned k = TopOrder[i];
		unsigned TargetIndex = TopTargetIndexes[k];
		unsigned WordCount = U[TargetIndex];
		asserta(WordCount <= LastWordCount);
		LastWordCount = WordCount;

		TargetIndexes[i] = TargetIndex;
		if (WordCounts != 0)
			WordCounts[i] = WordCount;
		}

	if (m_Self)
		N = DeleteSelf(Query, TargetIndexes, WordCounts, N);

	return N;
	}

unsigned UDBUsortedSearcher::GetHot(SeqInfo *Query, unsigned MaxHot, unsigned MaxDrop,
  unsigned *TargetIndexes)
	{
	m_Query = Query;

	SetQueryImpl();
	SetTargetOrder();

	unsigned N = m_TopOrder.Size;
	if (N == 0)
		return 0;

	if (N > MaxHot)
		N = MaxHot;

	const unsigned *TopOrder = m_TopOrder.Data;
	const unsigned *TopTargetIndexes = m_TopTargetIndexes.Data;
	const uint32 *U = m_U.Data;

	unsigned Topk = TopOrder[0];
	unsigned TopTargetIndex = TopTargetIndexes[Topk];
	unsigned TopWordCount = U[TopTargetIndex];
	TargetIndexes[0] = TopTargetIndex;
	for (unsigned i = 1; i < N; ++i)
		{
		unsigned k = TopOrder[i];
		unsigned TargetSeqIndex = TopTargetIndexes[k];
		unsigned WordCount = U[TargetSeqIndex];
		unsigned Drop = TopWordCount - WordCount;
		if (Drop > MaxDrop)
			return i;
		TargetIndexes[i] = TargetSeqIndex;
		}
	return N;
	}
