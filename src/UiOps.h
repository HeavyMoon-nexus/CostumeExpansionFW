#pragma once

#include <string>

// The SMF UI's entry into the SAME native-op bodies the Papyrus natives run
// (defined in Papyrus.cpp; these are forwarding wrappers ONLY - single
// implementation, two thin UIs, no drift). Every mutator keeps the CFW_Native
// contract (Papyrus.cpp:27): definition changes are synchronous, scene changes
// are queued to the main thread - so these are safe from the SMF render thread
// exactly as they are from the Papyrus VM thread.
namespace CostumeFW::UiOps
{
    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content);
    bool RemoveBoxContent(const std::string& a_token, const std::string& a_content, bool a_returnStored);
    bool RemoveBox(const std::string& a_token, bool a_returnStored);
    bool SetBoxEnabled(const std::string& a_token, bool a_on);
    bool SetBoxArmorType(const std::string& a_token, int a_type);
    bool SetContentGender(const std::string& a_id, int a_mode);
    bool SetHideSlotsStr(const std::string& a_id, const std::string& a_slots);
    std::string GetHideSlotsStr(const std::string& a_id);
    void SetHideShape(const std::string& a_id, const std::string& a_shape, bool a_on);
    void SetBodyMorph(const std::string& a_id, bool a_on);
    void SetShowRealBody(const std::string& a_id, bool a_on);
    void ScanContentShapes(const std::string& a_id);
    bool AssignPreset(const std::string& a_token, const std::string& a_file);
    bool ClearPreset(const std::string& a_token);
    std::string ExportPreset(const std::string& a_token, const std::string& a_name);
    std::string FindContentHolder(const std::string& a_id);
    bool ContentHasScript(const std::string& a_id);

    // Persist (P2b)
    bool AddPersist(const std::string& a_id);
    bool RemovePersist(const std::string& a_id, bool a_returnStored);
    bool SetPersistActive(const std::string& a_id, bool a_on);
    bool AssignPersistPreset(const std::string& a_file);
    bool ClearPersistPreset();
    std::string ExportPersist(const std::string& a_name);
}
