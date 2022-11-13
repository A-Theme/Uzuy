// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/applets/cabinet.h"
#include "core/hid/hid_core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applets/applet_cabinet.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/hle/service/nfp/nfp_device.h"

namespace Service::AM::Applets {

Cabinet::Cabinet(Core::System& system_, LibraryAppletMode applet_mode_,
                 const Core::Frontend::CabinetApplet& frontend_)
    : Applet{system_, applet_mode_}, frontend{frontend_}, system{system_}, service_context{
                                                                               system_,
                                                                               "CabinetApplet"} {

    availability_change_event =
        service_context.CreateEvent("CabinetApplet:AvailabilityChangeEvent");
}

Cabinet::~Cabinet() = default;

void Cabinet::Initialize() {
    Applet::Initialize();

    LOG_INFO(Service_HID, "Initializing Cabinet Applet.");

    LOG_ERROR(Service_HID,
              "Initializing Applet with common_args: arg_version={}, lib_version={}, "
              "play_startup_sound={}, size={}, system_tick={}, theme_color={}",
              common_args.arguments_version, common_args.library_version,
              common_args.play_startup_sound, common_args.size, common_args.system_tick,
              common_args.theme_color);

    const auto storage = broker.PopNormalDataToApplet();
    ASSERT(storage != nullptr);

    const auto applet_input_data = storage->GetData();
    ASSERT(applet_input_data.size() >= sizeof(StartParamForAmiiboSettings));

    std::memcpy(&applet_input_common, applet_input_data.data(),
                sizeof(StartParamForAmiiboSettings));
}

bool Cabinet::TransactionComplete() const {
    return is_complete;
}

Result Cabinet::GetStatus() const {
    return ResultSuccess;
}

void Cabinet::ExecuteInteractive() {
    ASSERT_MSG(false, "Attempted to call interactive execution on non-interactive applet.");
}

void Cabinet::Execute() {
    if (is_complete) {
        return;
    }

    const auto callback = [this](bool apply_changes, const std::string& amiibo_name) {
        DisplayCompleted(apply_changes, amiibo_name);
    };

    // TODO: listen on all controllers
    if (nfp_device == nullptr) {
        nfp_device = std::make_shared<Service::NFP::NfpDevice>(
            system.HIDCore().GetFirstNpadId(), system, service_context, availability_change_event);
        nfp_device->Initialize();
        nfp_device->StartDetection(Service::NFP::TagProtocol::All);
    }

    const Core::Frontend::CabinetParameters parameters{
        .tag_info = applet_input_common.tag_info,
        .register_info = applet_input_common.register_info,
        .mode = applet_input_common.applet_mode,
    };

    switch (applet_input_common.applet_mode) {
    case Service::NFP::CabinetMode::StartNicknameAndOwnerSettings:
    case Service::NFP::CabinetMode::StartGameDataEraser:
    case Service::NFP::CabinetMode::StartRestorer:
    case Service::NFP::CabinetMode::StartFormatter:
        frontend.ShowCabinetApplet(callback, parameters, nfp_device);
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown CabinetMode={}", applet_input_common.applet_mode);
        DisplayCompleted(false, {});
        break;
    }
}

void Cabinet::DisplayCompleted(bool apply_changes, const std::string& amiibo_name) {
    Service::Mii::MiiManager manager;
    ReturnValueForAmiiboSettings applet_output{};

    if (!apply_changes) {
        Cancel();
    }

    if (nfp_device->GetCurrentState() != Service::NFP::DeviceState::TagFound &&
        nfp_device->GetCurrentState() != Service::NFP::DeviceState::TagMounted) {
        Cancel();
    }

    if (nfp_device->GetCurrentState() != Service::NFP::DeviceState::TagFound) {
        nfp_device->Mount(Service::NFP::MountTarget::All);
    }

    switch (applet_input_common.applet_mode) {
    case Service::NFP::CabinetMode::StartNicknameAndOwnerSettings: {
        Service::NFP::AmiiboName name{};
        memccpy(name.data(), amiibo_name.data(), 0, name.size());
        nfp_device->SetNicknameAndOwner(name);
        break;
    }
    case Service::NFP::CabinetMode::StartGameDataEraser:
        nfp_device->DeleteApplicationArea();
        break;
    case Service::NFP::CabinetMode::StartRestorer:
        nfp_device->RestoreAmiibo();
        break;
    case Service::NFP::CabinetMode::StartFormatter:
        nfp_device->DeleteAllData();
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown CabinetMode={}", applet_input_common.applet_mode);
        break;
    }

    applet_output.device_handle = applet_input_common.device_handle;
    applet_output.result = CabinetResult::Success;
    nfp_device->GetRegisterInfo(applet_output.register_info);
    nfp_device->GetTagInfo(applet_output.tag_info);
    nfp_device->Finalize();

    std::vector<u8> out_data(sizeof(ReturnValueForAmiiboSettings));
    std::memcpy(out_data.data(), &applet_output, sizeof(ReturnValueForAmiiboSettings));

    is_complete = true;

    broker.PushNormalDataFromApplet(std::make_shared<IStorage>(system, std::move(out_data)));
    broker.SignalStateChanged();
}

void Cabinet::Cancel() {
    ReturnValueForAmiiboSettings applet_output{};
    applet_output.device_handle = applet_input_common.device_handle;
    applet_output.result = CabinetResult::Cancel;
    nfp_device->Finalize();

    std::vector<u8> out_data(sizeof(ReturnValueForAmiiboSettings));
    std::memcpy(out_data.data(), &applet_output, sizeof(ReturnValueForAmiiboSettings));

    is_complete = true;

    broker.PushNormalDataFromApplet(std::make_shared<IStorage>(system, std::move(out_data)));
    broker.SignalStateChanged();
}

} // namespace Service::AM::Applets
