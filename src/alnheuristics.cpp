#include "myutils.h"
#include "alnparams.h"
#include "alnheuristics.h"
#include "alpha.h"

AlnHeuristics::AlnHeuristics()
	{
	FullDPAlways = oget_flag(OPT_fulldp);
	BandRadius = 0;
	HSPFinderWordLength = 0;
	XDropG = 0.0f;
	XDropU = 0.0f;
	MinGlobalHSPLength = 0;
	}

void AlnHeuristics::LogMe() const
	{
	Log("AH: Band %u, HSPw %u, Xg %.1f, Xu %.1f, HSP %u\n",
	  BandRadius,
	  HSPFinderWordLength,
	  XDropG,
	  XDropU,
	  MinGlobalHSPLength);
	}

void AlnHeuristics::InitFromCmdLine(const AlnParams &AP)
	{
	FullDPAlways = oget_flag(OPT_fulldp);
	XDropU = (float) oget_flt(OPT_xdrop_u);
	XDropG = (float) oget_flt(OPT_xdrop_g);
	XDropGlobalHSP = (float) oget_flt(OPT_xdrop_nw);

	BandRadius = oget_uns(OPT_band);
	MinGlobalHSPLength = oget_uns(OPT_minhsp);

	if (AP.GetIsNucleo())
		{
		HSPFinderWordLength = 5;
		MinGlobalHSPFractId = max((float) oget_fltd(OPT_id, 0.5), 0.75f);
		MinGlobalHSPScore = MinGlobalHSPFractId*MinGlobalHSPLength*(float) oget_fltd(OPT_match, 1.0);
		}
	else
		{
		HSPFinderWordLength = 3;
		
	// Avg BLOSUM62 score on the diagonal is 5.2, for comparison
		const float * const *SubstMx = AP.SubstMx;
		float MinDiagScore = 9e9f;
		for (unsigned i = 0; i < 20; ++i)
			{
			byte c = g_LetterToCharAmino[i];
			float Score = SubstMx[c][c];
			if (Score < MinDiagScore)
				MinDiagScore = Score;
			}

		MinGlobalHSPFractId = max((float) oget_fltd(OPT_id, 0.5f), 0.5f);
		MinGlobalHSPScore = MinGlobalHSPFractId*MinDiagScore*MinGlobalHSPLength;
		}

	if (ofilled(OPT_hspw))
		HSPFinderWordLength = oget_uns(OPT_hspw);

	if (oget_flag(OPT_fulldp))
		{
		InitGlobalFullDP();
		return;
		}
	}

void AlnHeuristics::InitGlobalFullDP()
	{
	MinGlobalHSPLength = 0;
	HSPFinderWordLength = 0;
	BandRadius = 0;
	}
