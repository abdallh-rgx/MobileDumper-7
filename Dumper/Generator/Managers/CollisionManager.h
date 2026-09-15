#pragma once

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../Engine/Unreal/UnrealObjects.h"
#include "../HashStringTable.h"

enum class ECollisionType : uint8
{
	MemberName,
	SuperMemberName,
	FunctionName,
	SuperFunctionName,
	ParameterName,
	None,
};

inline std::string StringifyCollisionType(ECollisionType Type)
{
	switch (Type)
	{
	case ECollisionType::MemberName:
		return "ECollisionType::MemberName";
	case ECollisionType::SuperMemberName:
		return "ECollisionType::SuperMemberName";
	case ECollisionType::FunctionName:
		return "ECollisionType::FunctionName";
	case ECollisionType::SuperFunctionName:
		return "ECollisionType::SuperFunctionName";
	case ECollisionType::ParameterName:
		return "ECollisionType::ParameterName";
	case ECollisionType::None:
		return "ECollisionType::None";
	default:
		return "ECollisionType::Invalid";
	}
}

constexpr int32 OwnTypeBitCount  = 0x7;
constexpr int32 PerCountBitCount = 0x5;

struct NameInfo
{
public:
	HashStringTableIndex Name;

	union
	{
		struct
		{
			uint32 OwnType : OwnTypeBitCount;

			uint32 MemberNameCollisionCount : PerCountBitCount;
			uint32 SuperMemberNameCollisionCount : PerCountBitCount;
			uint32 FunctionNameCollisionCount : PerCountBitCount;
			uint32 SuperFuncNameCollisionCount : PerCountBitCount;
			uint32 ParamNameCollisionCount : PerCountBitCount;
		};

		uint32 CollisionData;
	};

	static_assert(sizeof(CollisionData) >= (OwnTypeBitCount + (5 * PerCountBitCount)) / 32, "Too many bits to fit into uint32, recude the number of bits!");

public:
	inline NameInfo()
	    : Name(HashStringTableIndex::FromInt(-1)),
	      CollisionData(0x0)
	{
	}

	NameInfo(HashStringTableIndex NameIdx, ECollisionType CurrentType);

public:
	void InitCollisionData(const NameInfo& Existing, ECollisionType CurrentType, bool bIsSuper);

	bool HasCollisions() const;

public:
	inline bool IsValid() const
	{
		return Name != -1;
	}

public:
	std::string DebugStringify() const;
};

namespace KeyFunctions
{
	uint64 GetKeyForCollisionInfo(UEStruct Super, UEProperty Member);
	uint64 GetKeyForCollisionInfo(UEStruct Super, UEFunction Function);
}


class CollisionManager
{
private:
	friend class CollisionManagerTest;
	friend class MemberManagerTest;

	friend class StructManager_NameAccessHelper;

public:
	using NameContainer = std::vector<NameInfo>;

	using NameInfoMapType    = std::unordered_map<uint64, NameContainer>;
	using TranslationMapType = std::unordered_map<uint64, uint64>;

private:
	HashStringTable MemberNames;

	CollisionManager::NameInfoMapType NameInfos;

	CollisionManager::TranslationMapType TranslationMap;

	NameContainer ClassReservedNames;

	NameContainer ReservedNames;

private:
	uint64 AddNameToContainer(NameContainer& StructNames, UEStruct Struct, std::pair<HashStringTableIndex, bool>&& NamePair, ECollisionType CurrentType, bool bIsStruct, UEFunction Func = nullptr);

public:
	void AddReservedClassName(const std::string& Name, bool bIsParameterOrLocalVariable);
	void AddReservedName(const std::string& Name);
	void AddStructToNameContainer(UEStruct ObjAsStruct, bool bIsStruct, bool bIsFunction = false);

	std::string StringifyName(UEStruct Struct, NameInfo Info);

public:
	template <typename UEType>
	inline NameInfo GetNameCollisionInfoUnchecked(UEStruct Struct, UEType Member)
	{
		auto StructIt = NameInfos.find(Struct.GetIndex());
		if (StructIt == NameInfos.end())
		{
			AddStructToNameContainer(
			    Struct,
			    (!Struct.IsA(EClassCastFlags::Class) && !Struct.IsA(EClassCastFlags::Function)),
			    Struct.IsA(EClassCastFlags::Function));

			StructIt = NameInfos.find(Struct.GetIndex());
			if (StructIt == NameInfos.end())
				return NameInfo();
		}

		NameContainer& InfosForStruct = StructIt->second;

		auto TransIt = TranslationMap.find(KeyFunctions::GetKeyForCollisionInfo(Struct, Member));
		if (TransIt == TranslationMap.end())
			return NameInfo();

		const uint64 NameInfoIndex = TransIt->second;
		if (NameInfoIndex >= InfosForStruct.size())
			return NameInfo();

		return InfosForStruct[NameInfoIndex];
	}

private:
	inline NameInfo& GetNameCollisionInfoRefUnchecked(UEStruct Struct, UEProperty Member)
	{
		static NameInfo Fallback;

		auto StructIt = NameInfos.find(Struct.GetIndex());
		if (StructIt == NameInfos.end())
		{
			AddStructToNameContainer(
			    Struct,
			    (!Struct.IsA(EClassCastFlags::Class) && !Struct.IsA(EClassCastFlags::Function)),
			    Struct.IsA(EClassCastFlags::Function));

			StructIt = NameInfos.find(Struct.GetIndex());
			if (StructIt == NameInfos.end())
				return Fallback;
		}

		NameContainer& InfosForStruct = StructIt->second;

		auto TransIt = TranslationMap.find(KeyFunctions::GetKeyForCollisionInfo(Struct, Member));
		if (TransIt == TranslationMap.end())
			return Fallback;

		const uint64 NameInfoIndex = TransIt->second;
		if (NameInfoIndex >= InfosForStruct.size())
			return Fallback;

		return InfosForStruct[NameInfoIndex];
	}
};

class StructManager_NameAccessHelper
{
private:
	friend class StructManager;

public:
	static inline void ReplaceName(CollisionManager& Collisions, UEStruct Struct, UEProperty Member, const std::string& NameToReplaceWith)
	{
		const auto [Index, _] = Collisions.MemberNames.FindOrAdd(NameToReplaceWith);

		auto& NameInfo = Collisions.GetNameCollisionInfoRefUnchecked(Struct, Member);

		NameInfo.CollisionData = 0;
		NameInfo.Name          = Index;
	}
};
