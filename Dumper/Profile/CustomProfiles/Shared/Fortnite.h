#pragma once

#include "../../IProfile.h"

class FortniteProfile : public IProfile
{
public:
    FortniteProfile() = default;

    std::vector<std::string> GetSupportedGames() const override
    {
        return {};
    }

    uintptr_t GetGObjects() const override
    {
        constexpr uintptr_t kGObjectsOffset = 0x10249200;
        return GMemory->GetUnrealModule().OffsetToAddress(kGObjectsOffset);
    }

    uintptr_t GetGNames() const override
    {
        constexpr uintptr_t kGNamesOffset = 0x101B5EC0;
        return GMemory->GetUnrealModule().OffsetToAddress(kGNamesOffset);
    }

    void OverrideInGenOffsets(FInGenOffsets& Offsets) const override
    {
        Offsets.UObject.Flags = 0x08;
        Offsets.UObject.Index = 0x0C;
        Offsets.UObject.Class = 0x10;
        Offsets.UObject.Name  = 0x18;
        Offsets.UObject.Outer = 0x20;

        Offsets.FName.CompIdx = 0x00;
        Offsets.FName.Number  = 0x04;
        Offsets.FName.SizeOf  = 0x08;

        Offsets.UField.Next = 0x28;

        Offsets.UStruct.StructBaseChain = 0x30;
        Offsets.UStruct.SuperStruct = 0x40;
        Offsets.UStruct.Children = 0x48;
        Offsets.UStruct.Size = 0x58;
        Offsets.UStruct.MinAlignment = 0x5C;
        Offsets.UStruct.ChildProperties = -1;

        Offsets.UClass.CastFlags = 0xE8;
        Offsets.UClass.ClassDefaultObject = 0x130;
        Offsets.UClass.ImplementedInterfaces = 0x220;

        Offsets.UEnum.Names = 0x40;
        Offsets.UEnum.UnderlyingType = -1;

        Offsets.UFunction.FunctionFlags = 0xC8;
        Offsets.UFunction.NumParams = 0xCC;
        Offsets.UFunction.ParamSize = 0xCE;
        Offsets.UFunction.ExecFunction = 0xF0;

        Offsets.Property.ArrayDim = 0x30;
        Offsets.Property.ElementSize = 0x34;
        Offsets.Property.PropertyFlags = 0x38;
        Offsets.Property.Offset_Internal = 0x44;

        Offsets.BoolProperty.Base = 0x70;
        Offsets.EnumProperty.Base = 0x70;
        Offsets.Property.SizeOf = 0x70;
        Offsets.ObjectProperty.PropertyClass = 0x70;
        Offsets.StructProperty.Struct = 0x70;
        Offsets.ArrayProperty.Inner = 0x70;
        Offsets.DelegateProperty.SignatureFunction = 0x70;
        Offsets.MapProperty.Base = 0x70;
        Offsets.SetProperty.ElementProp = 0x70;
        Offsets.ByteProperty.Enum = 0x70;

        Offsets.FField.Vft = -1;
        Offsets.FField.Class = -1;
        Offsets.FField.Owner = -1;
        Offsets.FField.Next = -1;
        Offsets.FField.Name = -1;
        Offsets.FField.EditorOnlyMetadata = -1;
        Offsets.FFieldClass.CastFlags = -1;
        Offsets.FFieldClass.Name = -1;

        InternalSettings::bUseFProperty = false;
    }

    void OverrideInSKOffsets(FInSDKOffsets& Offsets) const override
    {
        Offsets.Statics.GWorld  = 0x10191070;
        Offsets.Statics.GEngine = 0xDFDA008;
    }

    void DecryptUTF8(char* Data, int32_t Len) const override
    {
        if (!Data || Len == 0)
            return;
    }

    void DecryptUTF16(char16_t* Data, int32_t Len) const override
    {
        if (!Data || Len == 0)
            return;
    }
};
