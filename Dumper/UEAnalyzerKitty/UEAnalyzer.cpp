#include "UEAnalyzer.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <thread>

#include "../Architecture/IArchDecoder.h"

#include "Analysis/AnchoredResolution.h"
#include "Analysis/CallGraph.h"
#include "Analysis/GlobalAccessHarvester.h"
#include "Analysis/StringAnchors.h"
#include "Analysis/StructureVerifier.h"
#include "Analysis/TCharDetect.h"
#include "Strategies/IStrategy.h"

namespace UEAnalyzerKitty
{
	namespace
	{
		void ApplyValidator(std::vector<Candidate>& Candidates, const CandidateValidator& Validator)
		{
			if (!Validator)
				return;
			Candidates.erase(std::remove_if(Candidates.begin(), Candidates.end(), [&](const Candidate& C)
			{ return !Validator(C.Address); }),
			                 Candidates.end());
		}
	}

	struct UEAnalyzer::Analysis
	{
		GlobalAccessHarvester Harvester;

		struct TargetAnalysis
		{
			StrategyPtr Strategy;
			StringAnchors Anchors;
			std::vector<uint64_t> AnchorSites;
			std::vector<StringAnchors::WeightedSites> AnchorSitesByAnchor;
		};
		std::vector<TargetAnalysis> Targets;

		CallGraph Calls;

		const IArchDecoder* Arch = nullptr;

		ModuleInfo Module;

		ETCharKind TChar      = ETCharKind::Unknown;
		size_t TCharUtf16Hits = 0;
		size_t TCharUtf32Hits = 0;

		std::unordered_map<uint64_t, uint32_t> AccessRank;
		uint32_t RankedCount = 0;

		AnalysisContext MakeContext(const TargetAnalysis& Target, IMemory* Mem, const ScoringWeights& Weights) const
		{
			AnalysisContext Ctx;
			Ctx.Memory              = Mem;
			Ctx.Module              = &Module;
			Ctx.Harvester           = &Harvester;
			Ctx.AccessRank          = &AccessRank;
			Ctx.RankedCount         = RankedCount;
			Ctx.Weights             = Weights;
			Ctx.Anchors             = &Target.Anchors;
			Ctx.AnchorSites         = &Target.AnchorSites;
			Ctx.AnchorSitesByAnchor = &Target.AnchorSitesByAnchor;
			return Ctx;
		}

		void BuildRanking()
		{
			std::vector<std::pair<uint64_t, uint32_t>> Ranked;
			Ranked.reserve(Harvester.GetAccesses().size());
			for (const auto& [Address, Info] : Harvester.GetAccesses())
			{
				const MemRegionInfo* Seg = Module.FindAddressRegion(Address);
				if (!Seg || !Seg->IsWriteable() || Seg->IsExecutable())
					continue;
				Ranked.emplace_back(Address, Info.Count);
			}
			std::sort(Ranked.begin(), Ranked.end(), [](const auto& A, const auto& B)
			{
				if (A.second != B.second)
					return A.second > B.second;
				return A.first < B.first;
			});

			AccessRank.reserve(Ranked.size());
			for (size_t i = 0; i < Ranked.size(); ++i)
				AccessRank.emplace(Ranked[i].first, static_cast<uint32_t>(i) + 1u);
			RankedCount = static_cast<uint32_t>(Ranked.size());
		}
	};

	bool UEAnalyzer::IsValid() const
	{
		return State_ != nullptr;
	}

	UEAnalyzer UEAnalyzer::Analyze(IMemory* Memory, const IArchDecoder* Arch, AnalyzerOptions Options, HarvestOptions Harvest)
	{
		UEAnalyzer Out;
		Out.Options_ = std::move(Options);
		Out.Memory_  = Memory;

		if (!Memory)
		{
			Out.Error_ = "no memory backend";
			return Out;
		}
		if (!Memory->IsMemoryAccessOk())
		{
			Out.Error_ = "memory backend is not ready";
			return Out;
		}
		if (!Arch)
		{
			Out.Error_ = "no instruction decoder";
			return Out;
		}

		auto State    = std::make_shared<Analysis>();
		State->Module = Memory->GetUnrealModule();
		State->Arch   = Arch;

		{
			std::vector<StrategyPtr> Available = Targets::CreateAll();

			for (StrategyPtr& S : Available)
			{
				if (!Out.Options_.IsTargetWanted(S->Name()))
					continue;
				State->Targets.emplace_back().Strategy = std::move(S);
			}
		}

		const auto Harvest_ = [&]
		{ return State->Harvester.Run(Memory, Arch, Harvest, &State->Calls); };

		const auto ScanLiterals = [&]
		{
			for (Analysis::TargetAnalysis& T : State->Targets)
				T.Anchors.Run(Memory, State->Module, T.Strategy->ProximityAnchors());
		};

		bool bHarvested = false;

		if (Out.Options_.ThreadMode == EThreadMode::Two)
		{
			std::exception_ptr ScanError;
			{
				std::thread Scanner([&]
				{
					try
					{
						ScanLiterals();
					}
					catch (...)
					{
						ScanError = std::current_exception();
					}
				});
				bHarvested = Harvest_();
			}

			if (ScanError)
				std::rethrow_exception(ScanError);
		}
		else
		{
			bHarvested = Harvest_();
			ScanLiterals();
		}

		if (!bHarvested)
		{
			Out.Error_ = "harvest failed";
			return Out;
		}

		State->TChar = DetectTCharKind(Memory, &State->TCharUtf16Hits, &State->TCharUtf32Hits);

		for (Analysis::TargetAnalysis& T : State->Targets)
		{
			const std::unordered_set<uint64_t> Sites = T.Anchors.CollectAnchorSites(State->Harvester);
			T.AnchorSites.assign(Sites.begin(), Sites.end());
			std::sort(T.AnchorSites.begin(), T.AnchorSites.end());
			T.AnchorSitesByAnchor = T.Anchors.CollectAnchorSitesByAnchor(State->Harvester);
		}

		State->BuildRanking();

		Out.State_ = std::move(State);
		return Out;
	}

	namespace
	{
		const UEAnalyzer::Analysis::TargetAnalysis* FindTarget(const UEAnalyzer::Analysis& State, const char* TargetName)
		{
			if (!TargetName)
				return nullptr;
			for (const auto& Entry : State.Targets)
				if (std::strcmp(Entry.Strategy->Name(), TargetName) == 0)
					return &Entry;
			return nullptr;
		}
	}

	TargetAssessment UEAnalyzer::AssessAddress(const char* TargetName, uint64_t Address) const
	{
		TargetAssessment Out;
		Out.Target = TargetName ? TargetName : "";

		if (!State_)
		{
			Out.Reason = "analysis unavailable";
			return Out;
		}

		const Analysis::TargetAnalysis* Found = FindTarget(*State_, TargetName);
		if (!Found)
		{
			Out.Reason = "no strategy answers to this target";
			return Out;
		}
		Out.Target = Found->Strategy->Name();

		const MemRegionInfo* Seg = State_->Module.FindAddressRegion(static_cast<uintptr_t>(Address));
		if (!Seg)
		{
			Out.Reason = "not inside the module";
			return Out;
		}
		if (!Seg->IsWriteable() || Seg->IsExecutable())
		{
			Out.Reason = EVerdictToString(EVerdict::NotWritableData);
			return Out;
		}

		const AccessInfo* Info = State_->Harvester.Find(Address);
		if (!Info)
		{
			Out.Reason = EVerdictToString(EVerdict::Unreferenced);
			return Out;
		}

		const StructureVerifier Verifier(State_->Module, State_->Harvester);
		Out.Evidence = Found->Strategy->Verify(Verifier, Address);
		if (!Out.Evidence.Passed)
		{
			Out.Reason = "verification rejected it: " + Out.Evidence.Why;
			return Out;
		}
		Out.Layout = MakeLayoutDescription(Out.Evidence, Address);

		const LocateResult R = Run(Out.Target, {});
		Out.CandidateCount   = R.Candidates.size();
		for (size_t i = 0; i < R.Candidates.size(); ++i)
			if (R.Candidates[i].Address == Address)
			{
				Out.Rank = i + 1;
				break;
			}
		Out.bAnswer = R.bVerified && Out.Rank == 1;

		char Buf[192];
		if (Out.Rank)
		{
			std::snprintf(Buf, sizeof(Buf), "verified, rank %zu of %zu%s", Out.Rank, Out.CandidateCount, Out.bAnswer ? " - returned as the answer" : "");
			Out.Reason = Buf;
			return Out;
		}

		AnalysisContext Ctx                 = State_->MakeContext(*Found, Memory_, Options_.Weights);
		Ctx.MaxScored                       = 0;
		const std::vector<Candidate> Scored = Scoring::FuseCandidates(Found->Strategy->Score(Ctx), Options_.Weights);

		size_t FullRank = 0;
		for (size_t i = 0; i < Scored.size(); ++i)
			if (Scored[i].Address == Address)
			{
				FullRank = i + 1;
				break;
			}

		if (FullRank)
			std::snprintf(Buf, sizeof(Buf), "verified, scored rank %zu of %zu - the strategy keeps %zu (%u accesses, %u pointer-width)", FullRank, Scored.size(), kMaxScoredCandidates, Info->Count, Info->WideAccesses);
		else
			std::snprintf(Buf, sizeof(Buf), "verified, but a scoring filter rejected it (%u accesses, %u pointer-width)", Info->Count, Info->WideAccesses);
		Out.Reason = Buf;
		return Out;
	}

	AddressExplanation UEAnalyzer::Explain(uint64_t Address) const
	{
		AddressExplanation Out;
		if (!State_)
			return Out;

		Out.Address = Address;

		if (const MemRegionInfo* Seg = State_->Module.FindAddressRegion(static_cast<uintptr_t>(Address)))
		{
			Out.bInModule   = true;
			Out.Segment     = Seg->GetPathName();
			Out.bWritable   = Seg->IsWriteable();
			Out.bExecutable = Seg->IsExecutable();
		}

		if (const AccessInfo* Info = State_->Harvester.Find(Address))
		{
			Out.bReferenced          = true;
			Out.Access.Total         = Info->Count;
			Out.Access.PointerWide   = Info->WideAccesses;
			Out.Access.Narrow        = Info->NarrowAccesses;
			Out.Access.RankedTotal   = State_->RankedCount;
			Out.Access.bAddressTaken = Info->HasKind(EAccessKind::AddressTaken);
			Out.Access.bIndirectBase = Info->HasKind(EAccessKind::IndirectBase);
			Out.Access.bLoaded       = Info->HasKind(EAccessKind::Load);
			Out.Access.bStored       = Info->HasKind(EAccessKind::Store);
			Out.Access.Offsets.assign(Info->Offsets.begin(), Info->Offsets.end());

			const auto It   = State_->AccessRank.find(Address);
			Out.Access.Rank = It == State_->AccessRank.end() ? 0u : It->second;
		}

		Out.Targets.reserve(State_->Targets.size());
		for (const Analysis::TargetAnalysis& T : State_->Targets)
			Out.Targets.push_back(AssessAddress(T.Strategy->Name(), Address));

		return Out;
	}

	std::vector<std::string> UEAnalyzer::TargetNames() const
	{
		std::vector<std::string> Names;
		if (!State_)
			return Names;

		Names.reserve(State_->Targets.size());
		for (const Analysis::TargetAnalysis& T : State_->Targets)
			Names.emplace_back(T.Strategy->Name());
		return Names;
	}

	LayoutDescription UEAnalyzer::MakeLayoutDescription(const StructureEvidence& Evidence, uint64_t Address) const
	{
		LayoutDescription Out;
		Out.Layout = Evidence.Layout;

		if (Evidence.PrimaryFieldOffset >= 0)
		{
			Out.Table.Offset = Evidence.PrimaryFieldOffset;
			Out.Table.Reads  = Evidence.PrimaryFieldAccesses;
			Out.Table.Width  = static_cast<uint8_t>(sizeof(uintptr_t));
			Out.bValid       = true;
		}

		for (int64_t Off : Evidence.Fields)
		{
			const AccessInfo* Info = State_->Harvester.Find(Address + static_cast<uint64_t>(Off));
			if (!Info || Info->NarrowAccesses == 0)
				continue;
			Out.Counts.push_back(LayoutField{Off, Info->NarrowAccesses, 4});
			Out.bValid = true;
		}
		return Out;
	}

	ETCharKind UEAnalyzer::GetTCharKind() const
	{
		return State_ ? State_->TChar : ETCharKind::Unknown;
	}

	size_t UEAnalyzer::GetTCharSize() const
	{
		switch (GetTCharKind())
		{
		case ETCharKind::Char16:
			return 2;
		case ETCharKind::Char32:
			return 4;
		default:
			return 0;
		}
	}

	LocateResult UEAnalyzer::Find(const char* TargetName, const CandidateValidator& Validator) const
	{
		return Run(TargetName, Validator);
	}

	LocateResult UEAnalyzer::Run(const char* TargetName, const CandidateValidator& Validator) const
	{
		LocateResult Result;

		if (!State_ || !Memory_ || !TargetName)
			return Result;

		Analysis* State = State_.get();

		const Analysis::TargetAnalysis* Found = nullptr;
		for (const Analysis::TargetAnalysis& Entry : State->Targets)
		{
			if (std::strcmp(Entry.Strategy->Name(), TargetName) == 0)
			{
				Found = &Entry;
				break;
			}
		}
		if (!Found)
			return Result;

		const Analysis::TargetAnalysis& T = *Found;
		const IStrategy& Strategy         = *T.Strategy;
		Result.Target                     = Strategy.Name();

		AnalysisContext Ctx = State->MakeContext(T, Memory_, Options_.Weights);
		Ctx.MaxScored = std::max(Options_.MaxCandidates, kMaxScoredCandidates);

		const bool bWantAnchored = !Options_.Method || *Options_.Method == EFindMethod::Anchored;
		const bool bWantLeads    = !Options_.Method || *Options_.Method == EFindMethod::Statistical;

		if (State->Arch && bWantAnchored)
		{
			AnchoredResolution Anchored(Memory_, State->Module, State->Harvester, *State->Arch, &State->Calls);
			std::vector<Candidate> Verified = Anchored.Resolve(Strategy, Options_.Weights);

			Verified.erase(std::remove_if(Verified.begin(), Verified.end(), [&](const Candidate& C)
			{ return C.Confidence < Options_.Weights.MinConfidence; }),
			               Verified.end());

			ApplyValidator(Verified, Validator);

			if (!Verified.empty())
			{
				Result.Candidates = std::move(Verified);
				Result.bVerified  = Strategy.ClaimsVerification();
				Result.FindMethod = EFindMethod::Anchored;
				if (Result.Candidates.size() > Options_.MaxCandidates)
					Result.Candidates.resize(Options_.MaxCandidates);
				return Result;
			}
		}

		if (!bWantLeads)
			return Result;

		Result.Candidates = Scoring::FuseCandidates(Strategy.Score(Ctx), Options_.Weights);
		ApplyValidator(Result.Candidates, Validator);
		Result.bVerified  = false;
		Result.FindMethod = EFindMethod::Statistical;
		if (Result.Candidates.size() > Options_.MaxCandidates)
			Result.Candidates.resize(Options_.MaxCandidates);

		return Result;
	}

}
