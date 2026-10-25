#include "hwio/acquisition.hpp"
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <algorithm>
#include <cwctype>

namespace hwio {
UsbInputInfo inspect_usb_input(const std::wstring &path) {
    UsbInputInfo result;
    result.detail = "USB ancestry unavailable";
    HDEVINFO devices = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devices == INVALID_HANDLE_VALUE)
        return result;
    SP_DEVICE_INTERFACE_DATA face{};
    face.cbSize = sizeof(face);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    DWORD needed{};
    bool found = false;
    if (SetupDiOpenDeviceInterfaceW(devices, path.c_str(), 0, &face)) {
        SetupDiGetDeviceInterfaceDetailW(devices, &face, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buffer(needed);
        if (needed >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
            detail->cbSize = sizeof(*detail);
            found = SetupDiGetDeviceInterfaceDetailW(devices, &face, detail, needed, nullptr, &device) != FALSE;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (!found)
        return result;
    DEVINST node = device.DevInst, hub_node{};
    for (unsigned depth = 0; depth < 16; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            return result;
        std::wstring upper(id);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return std::towupper(c); });
        // Match the physical USB ancestor, never VID/PID alone (several identical boards are valid).
        if (upper.find(L"USB\\VID_") == 0 && upper.find(L"&MI_") == std::wstring::npos) {
            result.usb = true;
            break;
        }
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
            return result;
        node = parent;
    }
    if (!result.usb)
        return result;
    result.detail = "USB interval unavailable";
    ULONG port{}, type{}, length = sizeof(port);
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_ADDRESS, &type, &port, &length, 0) != CR_SUCCESS ||
        type != REG_DWORD || !port || CM_Get_Parent(&hub_node, node, 0) != CR_SUCCESS)
        return result;
    wchar_t hub_id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(hub_node, hub_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
        return result;
    GUID guid = GUID_DEVINTERFACE_USB_HUB;
    ULONG chars{};
    if (CM_Get_Device_Interface_List_SizeW(&chars, &guid, hub_id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        chars < 2)
        return result;
    std::vector<wchar_t> paths(chars);
    if (CM_Get_Device_Interface_ListW(&guid, hub_id, paths.data(), chars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS)
        return result;
    HANDLE hub = CreateFileW(paths.data(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hub == INVALID_HANDLE_VALUE) {
        result.detail += " (hub open error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    alignas(USB_NODE_CONNECTION_INFORMATION_EX) BYTE storage[8192]{};
    auto info = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX *>(storage);
    info->ConnectionIndex = port;
    DWORD returned{};
    const bool ok = DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, storage, sizeof(storage),
                                    storage, sizeof(storage), &returned, nullptr) != FALSE;
    CloseHandle(hub);
    if (!ok || returned < offsetof(USB_NODE_CONNECTION_INFORMATION_EX, PipeList) ||
        info->ConnectionStatus != DeviceConnected)
        return result;
    result.speed = info->Speed;
    unsigned inputs = 0;
    for (ULONG i = 0;
         i < info->NumberOfOpenPipes &&
         offsetof(USB_NODE_CONNECTION_INFORMATION_EX, PipeList) + (i + 1) * sizeof(USB_PIPE_INFO) <= returned;
         ++i) {
        const auto &endpoint = info->PipeList[i].EndpointDescriptor;
        if ((endpoint.bmAttributes & 3) != 3 || !(endpoint.bEndpointAddress & 0x80))
            continue;
        ++inputs;
        result.endpoint = endpoint.bEndpointAddress;
        result.packet_bytes = endpoint.wMaxPacketSize;
        result.interval = endpoint.bInterval;
    }
    if (inputs != 1) {
        result.interval = result.endpoint = result.packet_bytes = 0;
        result.detail = inputs ? "USB interval unavailable: multiple interrupt IN endpoints"
                               : "USB interrupt IN endpoint unavailable";
        return result;
    }
    result.interval_ms = usb_descriptor_interval_ms(result.speed, result.interval);
    result.detail =
        result.interval_ms ? "USB descriptor interval (not measured sensor rate)" : "USB interval encoding unavailable";
    return result;
}
} // namespace hwio
