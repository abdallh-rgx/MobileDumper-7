#pragma once

#include "../../Engine/Unreal/Enums.h"
#include "../../Engine/Unreal/UnrealObjects.h"
#include "../../Engine/Unreal/ObjectArray.h"
#include "../../Memory/IMemory.h"

#include "../HashStringTable.h"

#include "DependencyManager.h"


namespace PackageManagerUtils
{
	std::unordered_set<int32> GetDependencies(UEStruct Struct, int32 StructIndex);
}

class PackageInfoHandle;
class PackageManager;

struct RequirementInfo
{
	int32 PackageIdx;
	bool bShouldIncludeStructs;
	bool bShouldIncludeClasses;
};

struct VisitedNodeInformation
{
	int32 PackageIdx;
	mutable uint64 StructsIterationHitCount = 0x0;
	mutable uint64 ClassesIterationHitCount = 0x0;
};

using DependencyListType = std::unordered_map<int32, RequirementInfo>;


struct DependencyInfo
{
	mutable uint64 StructsIterationHitCount = 0x0;
	mutable uint64 ClassesIterationHitCount = 0x0;

	DependencyListType StructsDependencies;
	DependencyListType ClassesDependencies;
	DependencyListType ParametersDependencies;
};

struct IncludeData
{
	bool bIncludedStructs = false;
	bool bIncludedClasses = false;
};

using VisitedNodeContainerType = std::unordered_map<int32, IncludeData>;


struct PackageInfo
{
private:
	friend class PackageInfoHandle;
	friend class PackageManager;
	friend class PackageManagerTest;

private:
	int32 PackageIndex;

	HashStringTableIndex Name = HashStringTableIndex::FromInt(-1);

	uint64 CollisionCount = 0x0;

	bool bHasParams;

	DependencyManager StructsSorted;
	DependencyManager ClassesSorted;

	std::vector<int32> Functions;
	std::vector<int32> Enums;

	std::vector<std::pair<int32, bool>> EnumForwardDeclarations;

	mutable DependencyInfo PackageDependencies;
};

class PackageInfoHandle
{
private:
	const PackageInfo* Info;

public:
	PackageInfoHandle() = default;
	PackageInfoHandle(std::nullptr_t Nullptr);
	PackageInfoHandle(const PackageInfo& InInfo);

public:
	inline bool IsValidHandle() { return Info != nullptr; }

public:
	int32 GetIndex() const;

	std::string GetName() const;
	const StringEntry& GetNameEntry() const;
	std::pair<std::string, uint8> GetNameCollisionPair() const;

	bool HasClasses() const;
	bool HasStructs() const;
	bool HasFunctions() const;
	bool HasParameterStructs() const;
	bool HasEnums() const;

	bool IsEmpty() const;

	const DependencyManager& GetSortedStructs() const;
	const DependencyManager& GetSortedClasses() const;

	const std::vector<int32>& GetFunctions() const;
	const std::vector<int32>& GetEnums() const;

	const std::vector<std::pair<int32, bool>>& GetEnumForwardDeclarations() const;

	const DependencyInfo& GetPackageDependencies() const;

	void ErasePackageDependencyFromStructs(int32 Package) const;
	void ErasePackageDependencyFromClasses(int32 Package) const;
};

using PackageManagerOverrideMapType = std::unordered_map<int32 /* PackageIndex */, PackageInfo>;

struct PackageInfoIterator
{
private:
	friend class PackageManager;

private:
	using MapType      = PackageManagerOverrideMapType;
	using IteratorType = PackageManagerOverrideMapType::const_iterator;

private:
	const MapType& PackageInfos;
	uint8 CurrentIterationHitCount;
	IteratorType It;

private:
	explicit PackageInfoIterator(const MapType& Infos, uint8 IterationHitCount, IteratorType ItPos)
	    : PackageInfos(Infos),
	      CurrentIterationHitCount(IterationHitCount),
	      It(ItPos)
	{
	}

	explicit PackageInfoIterator(const MapType& Infos, uint8 IterationHitCount)
	    : PackageInfos(Infos),
	      CurrentIterationHitCount(IterationHitCount),
	      It(Infos.cbegin())
	{
	}

public:
	inline PackageInfoIterator& operator++()
	{
		++It;
		return *this;
	}
	inline PackageInfoHandle operator*() const { return {PackageInfoHandle(It->second)}; }

	inline bool operator==(const PackageInfoIterator& Other) const { return It == Other.It; }
	inline bool operator!=(const PackageInfoIterator& Other) const { return It != Other.It; }

public:
	PackageInfoIterator begin() const { return PackageInfoIterator(PackageInfos, CurrentIterationHitCount, PackageInfos.cbegin()); }
	PackageInfoIterator end() const { return PackageInfoIterator(PackageInfos, CurrentIterationHitCount, PackageInfos.cend()); }
};

struct PackageManagerIterationParams
{
	int32 PrevPackage;
	int32 RequiredPackage;

	bool bWasPrevNodeStructs;
	bool bRequiresClasses;
	bool bRequiresStructs;

	VisitedNodeContainerType& VisitedNodes;
};

class PackageManager
{
private:
	friend class PackageInfoHandle;
	friend class PackageManagerTest;

public:
	using OverrideMaptType = PackageManagerOverrideMapType;

	using IteratePackagesCallbackType = std::function<void(const PackageManagerIterationParams& OldParams, const PackageManagerIterationParams& NewParams, bool bIsStruct)>;
	using FindCycleCallbackType       = std::function<void(const PackageManagerIterationParams& OldParams, const PackageManagerIterationParams& NewParams, bool bIsStruct)>;

private:
	struct SingleDependencyIterationParamsInternal
	{
		const IteratePackagesCallbackType& CallbackForEachPackage;
		const FindCycleCallbackType& OnFoundCycle;

		PackageManagerIterationParams& NewParams;
		const PackageManagerIterationParams& OldParams;
		const DependencyListType& Dependencies;
		VisitedNodeContainerType& VisitedNodes;

		int32 CurrentIndex;
		int32 PrevIndex;
		uint64& IterationHitCounterRef;

		bool bShouldHandlePackage;
		bool bIsStruct;
	};

private:
	static inline HashStringTable UniquePackageNameTable;

	static inline OverrideMaptType PackageInfos;

	static inline uint64 CurrentIterationHitCount = 0x0;

	static inline bool bIsInitialized     = false;
	static inline bool bIsPostInitialized = false;

private:
	static void InitDependencies();
	static void InitNames();
	static void HandleCycles();

private:
	static void HelperMarkStructDependenciesOfPackage(UEStruct Struct, int32 OwnPackageIdx, int32 RequiredPackageIdx, bool bIsClass);
	static int32 HelperCountStructDependenciesOfPackage(UEStruct Struct, int32 OwnPackageIdx, bool bIsClass);

	static void HelperAddEnumsFromPacakageToFwdDeclarations(UEStruct Struct, std::vector<std::pair<int32, bool>>& EnumsToForwardDeclare, int32 RequiredPackageIdx, bool bMarkAsClass);

	static void HelperInitEnumFwdDeclarationsForPackage(int32 PackageForFwdDeclarations, int32 RequiredPackage, bool bIsClass);

public:
	static void Init();
	static void PostInit();

private:
	static inline const StringEntry& GetPackageName(const PackageInfo& Info)
	{
		return UniquePackageNameTable[Info.Name];
	}

private:
	static void IterateSingleDependencyImplementation(SingleDependencyIterationParamsInternal& Params, bool bCheckForCycle);

	static void IterateDependenciesImplementation(const PackageManagerIterationParams& Params, const IteratePackagesCallbackType& CallbackForEachPackage, const FindCycleCallbackType& OnFoundCycle, bool bCheckForCycle);

public:
	static void IterateDependencies(const IteratePackagesCallbackType& CallbackForEachPackage);
	static void FindCycle(const FindCycleCallbackType& OnFoundCycle);

public:
	static inline const OverrideMaptType& GetPackageInfos()
	{
		return PackageInfos;
	}

	static inline std::string GetName(int32 PackageIndex)
	{
		return GetInfo(PackageIndex).GetName();
	}

	static inline bool IsPackageNameUnique(const PackageInfo& Info)
	{
		return UniquePackageNameTable[Info.Name].IsUnique();
	}

	static inline PackageInfoHandle GetInfo(int32 PackageIndex)
	{
		auto It = PackageInfos.find(PackageIndex);
		if (It != PackageInfos.end())
			return It->second;

		// On-demand registration for a package that was not present during Init().
		// Some assets (e.g. AnimBlueprints) contain struct/class definitions without
		// being reachable as Struct/Class/Function/Enum in the first pass, so they
		// never entered PackageInfos. Without this fallback, .at() would throw
		// std::out_of_range and abort SDK generation.
		PackageInfo& NewInfo = PackageInfos[PackageIndex];
		NewInfo.PackageIndex = PackageIndex;

		std::string ResolvedName;
		if (GMemory && GMemory->IsMemoryAccessOk())
		{
			UEObject PackageObject = ObjectArray::GetByIndex(PackageIndex);
			if (PackageObject)
				ResolvedName = PackageObject.GetValidName();
		}

		if (ResolvedName.empty())
			ResolvedName = "UnknownPackage_" + std::to_string(PackageIndex);

		auto [NameIdx, bInserted] = UniquePackageNameTable.FindOrAdd(ResolvedName);
		NewInfo.Name = NameIdx;

		return NewInfo;
	}

	static inline PackageInfoHandle GetInfo(const UEObject Package)
	{
		if (!Package)
			return {};

		return GetInfo(Package.GetIndex());
	}

	static inline PackageInfoIterator IterateOverPackageInfos()
	{
		CurrentIterationHitCount++;

		return PackageInfoIterator(PackageInfos, CurrentIterationHitCount);
	}
};
