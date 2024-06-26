#ifndef seqsource_h
#define seqsource_h

#include <mutex>
#include "filetype.h"

class SeqInfo;
class ObjMgr;

class SeqSource
	{
public:
	static mutex m_Lock;
	static void LOCK_CLASS() { m_Lock.lock(); }
	static void UNLOCK_CLASS() { m_Lock.unlock(); }

public:
	unsigned m_SeqCount;
	bool m_EndOfFile;

public:
	SeqSource();
	virtual ~SeqSource();

public:
	virtual bool GetIsNucleo() = 0;
	virtual double GetPctDone() = 0;
	virtual const char *GetFileNameC() const = 0;
	virtual void Rewind() = 0;

public:
	virtual bool GetNextLo(SeqInfo *SI) = 0;

public:
	bool GetNext(SeqInfo *SI);
	};

SeqSource *MakeSeqSource(const string &FileName);

#endif // seqsource_h
