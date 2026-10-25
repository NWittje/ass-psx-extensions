#include "hwio/hid.hpp"
#include <hidsdi.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <psx/diagnostics/journal.hpp>
#include <algorithm>
#include <sstream>
#include <set>
#include <cstring>
#include <objbase.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace hwio {
constexpr UINT fallback_ready_message = WM_APP + 17;
void AtomicSample::store(const Sample &s) noexcept {
    serial.fetch_add(1, std::memory_order_acq_rel);
    generation.store(s.generation, std::memory_order_relaxed);
    sequence.store(s.sequence, std::memory_order_relaxed);
    qpc.store(s.qpc, std::memory_order_relaxed);
    value.store(s.value, std::memory_order_relaxed);
    invalid.store(s.invalid,std::memory_order_relaxed);
    serial.fetch_add(1, std::memory_order_release);
}
bool AtomicSample::read(Sample &s) const noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto before = serial.load(std::memory_order_acquire);
        if (before & 1)
            continue;
        s.generation = generation.load(std::memory_order_relaxed);
        s.sequence = sequence.load(std::memory_order_relaxed);
        s.qpc = qpc.load(std::memory_order_relaxed);
        s.value = value.load(std::memory_order_relaxed);
        s.invalid=invalid.load(std::memory_order_relaxed);
        auto after = serial.load(std::memory_order_acquire);
        if (before == after)
            return s.sequence != 0;
    }
    return false;
}
bool report_batch(std::size_t available, std::uint32_t size, std::uint32_t count) noexcept {
    return size != 0 && count != 0 && static_cast<std::uint64_t>(size) * count <= available;
}
bool raw_block_size(std::size_t remaining, std::uint32_t size) noexcept {
    return size >= sizeof(RAWINPUTHEADER) && size <= remaining;
}
void decode_report(const std::vector<ControlInfo> &controls, std::uint8_t id, const ReportValue *values,
                   std::size_t count, std::array<std::int32_t, max_controls> &out,
                   std::array<bool, max_controls> &present) noexcept {
    present.fill(false);
    for (std::size_t i = 0; i < controls.size() && i < max_controls; ++i) {
        const auto &c = controls[i];
        if (c.key.report_id != id)
            continue;
        if (c.key.kind == ControlKind::button) {
            out[i] = 0;
            present[i] = true;
        }
        for (std::size_t j = 0; j < count; ++j)
            if (values[j].index == c.key.data_index) {
                auto v = values[j].value;
                if (c.key.kind == ControlKind::button)
                    v = (v & 255) ? 1 : 0;
                else if (c.minimum < 0 && c.bits > 0 && c.bits < 32 && (v & (1u << (c.bits - 1))))
                    v |= (~0u << c.bits);
                out[i] = static_cast<std::int32_t>(v);
                present[i] = true;
                break;
            }
    }
}
static std::string hash_descriptor(const std::vector<ControlInfo> &controls, const HIDP_CAPS &caps) {
    std::uint64_t hash = 14695981039346656037ULL;
    auto add = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (v >> (i * 8)) & 255;
            hash *= 1099511628211ULL;
        }
    };
    add(caps.UsagePage);
    add(caps.Usage);
    add(caps.InputReportByteLength);
    for (const auto &c : controls) {
        add(static_cast<unsigned>(c.key.kind));
        add(c.key.usage_page);
        add(c.key.usage);
        add(c.key.link_collection);
        add(c.key.report_id);
        add(c.key.data_index);
        add(static_cast<unsigned>(c.minimum));
        add(static_cast<unsigned>(c.maximum));
        add(c.bits);
        add(c.relative);
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}
static std::string container_id(const std::wstring &path) {
    std::string result;
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE)
        return result;
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    if (SetupDiOpenDeviceInterfaceW(set, path.c_str(), 0, &interface_data)) {
        DWORD needed{};
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &needed, nullptr);
        std::vector<std::uint8_t> buffer(needed);
        if (needed >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
            detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            if (SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, needed, nullptr, &info)) {
                GUID guid{};
                DEVPROPTYPE type{};
                if (SetupDiGetDevicePropertyW(set, &info, &DEVPKEY_Device_ContainerId, &type,
                                              reinterpret_cast<PBYTE>(&guid), sizeof(guid), nullptr, 0) &&
                    type == DEVPROP_TYPE_GUID) {
                    wchar_t value[64]{};
                    StringFromGUID2(guid, value, 64);
                    result = utf8(value);
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}
static std::string control_name(const ControlInfo &c) {
    const char *generic = nullptr;
    if (c.key.usage_page == 1) {
        switch (c.key.usage) {
        case 0x30:
            generic = "X Axis";
            break;
        case 0x31:
            generic = "Y Axis";
            break;
        case 0x32:
            generic = "Z Axis";
            break;
        case 0x33:
            generic = "X Rotation";
            break;
        case 0x34:
            generic = "Y Rotation";
            break;
        case 0x35:
            generic = "Z Rotation";
            break;
        case 0x36:
            generic = "Slider";
            break;
        case 0x37:
            generic = "Dial";
            break;
        case 0x39:
            generic = "Hat";
            break;
        default:
            break;
        }
    }
    if (generic)
        return generic;
    if (c.key.kind == ControlKind::button)
        return "Button " + std::to_string(c.key.usage);
    return "Control " + std::to_string(c.key.usage_page) + ":" + std::to_string(c.key.usage);
}
struct WindowsNames {
    IDirectInput8W *input{};
    std::wstring path;
    std::vector<DIDEVICEOBJECTINSTANCEW> objects;
};
static BOOL CALLBACK object_name(const DIDEVICEOBJECTINSTANCEW *object, void *context) {
    static_cast<WindowsNames *>(context)->objects.push_back(*object);
    return DIENUM_CONTINUE;
}
static BOOL CALLBACK device_names(const DIDEVICEINSTANCEW *device, void *context) {
    auto &names = *static_cast<WindowsNames *>(context);
    IDirectInputDevice8W *input = nullptr;
    if (FAILED(names.input->CreateDevice(device->guidInstance, &input, nullptr)))
        return DIENUM_CONTINUE;
    DIPROPGUIDANDPATH path{};
    path.diph.dwSize = sizeof(path);
    path.diph.dwHeaderSize = sizeof(path.diph);
    path.diph.dwHow = DIPH_DEVICE;
    const bool match = SUCCEEDED(input->GetProperty(DIPROP_GUIDANDPATH, &path.diph)) &&
                       _wcsicmp(path.wszPath, names.path.c_str()) == 0;
    if (match)
        input->EnumObjects(object_name, &names, DIDFT_ALL);
    input->Release();
    return match ? DIENUM_STOP : DIENUM_CONTINUE;
}
static void windows_names(DeviceInfo &device, const std::wstring &path) {
    WindowsNames names;
    names.path = path;
    if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                     reinterpret_cast<void **>(&names.input), nullptr))) {
        names.input->EnumDevices(DI8DEVCLASS_GAMECTRL, device_names, &names, DIEDFL_ATTACHEDONLY);
        names.input->Release();
    }
    for (auto &control : device.controls) {
        std::vector<const DIDEVICEOBJECTINSTANCEW *> candidates;
        for (const auto &object : names.objects)
            if (object.wUsagePage == control.key.usage_page && object.wUsage == control.key.usage &&
                object.wCollectionNumber == control.key.link_collection)
                candidates.push_back(&object);
        if (candidates.size() == 1)
            control.name = utf8(candidates.front()->tszName);
        control.name = device.identity.name + " - " + control.name;
    }
    std::map<std::string, int> duplicates;
    for (const auto &control : device.controls)
        ++duplicates[control.name];
    for (auto &control : device.controls)
        if (duplicates[control.name] > 1)
            control.name += " [report " + std::to_string(control.key.report_id) + ", collection " +
                            std::to_string(control.key.link_collection) + ", index " +
                            std::to_string(control.key.data_index) + "]";
}
HidInput::~HidInput() {
    stop();
    if (available_event)
        CloseHandle(available_event);
    if (stop_event_)
        CloseHandle(stop_event_);
    if (discover_event_)
        CloseHandle(discover_event_);
    if (discovered_event_)
        CloseHandle(discovered_event_);
}
void HidInput::start() {
    if (!thread_.joinable()) {
        if (!available_event || !stop_event_ || !discover_event_ || !discovered_event_) {
            ++rejected;
            return;
        }
        stop_ = false;
        ResetEvent(stop_event_);
        thread_ = std::thread(&HidInput::run, this);
    }
}
void HidInput::stop() {
    stop_ = true;
    SetEvent(stop_event_);
    const auto tid = thread_id_.load();
    if (tid)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    if (thread_.joinable())
        thread_.join();
}
std::vector<DeviceInfo> HidInput::devices() {
    std::lock_guard<std::mutex> guard(metadata_mutex_);
    return metadata_;
}
bool HidInput::latest(std::size_t d, std::size_t c, Sample &value) const noexcept {
    if (d >= max_usb_devices || c >= max_usb_controls)
        return false;
    value.device = static_cast<std::uint16_t>(d);
    value.control = static_cast<std::uint16_t>(c);
    return latest_[d][c].read(value);
}
InputActivity HidInput::activity(std::size_t device) const noexcept {
    if (device >= max_usb_devices)
        return {};
    const auto &a = activity_[device];
    return {a.reports.load(), a.packets.load(), a.last_qpc.load()};
}
void HidInput::publish_metadata() {
    std::vector<DeviceInfo> list;
    for (const auto &d : devices_)
        if (!d.info.identity.name.empty())
            list.push_back(d.info);
    {
        std::lock_guard<std::mutex> guard(metadata_mutex_);
        metadata_ = std::move(list);
    }
    ++metadata_revision;
    SetEvent(available_event);
}
void HidInput::remove(HANDLE handle) {
    for (auto &d : devices_)
        if (d.handle == handle) {
            d.stream.reset();
            d.handle = nullptr;
            d.info.direct = false;
            d.raw_ready = false;
            d.info.connected = false;
            d.info.status = "Disconnected";
            d.info.generation = ++generation_;
            d.known.fill(false);
            publish_metadata();
            return;
        }
}
void HidInput::discover() {
    // Runs on the discovery worker, never on the report/completion thread.
    const auto existing = devices();
    std::vector<Cached> discovered;
    UINT count{};
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0)
        return;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1))
        return;
    list.resize(count);
    for (const auto &item : list) {
        if (stop_)
            return;
        if (item.dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO rawinfo{};
        rawinfo.cbSize = sizeof(rawinfo);
        UINT size = sizeof(rawinfo);
        if (GetRawInputDeviceInfoW(item.hDevice, RIDI_DEVICEINFO, &rawinfo, &size) == UINT(-1) ||
            !joystick_collection(rawinfo.hid.usUsagePage, rawinfo.hid.usUsage))
            continue;
        Cached d;
        d.handle = item.hDevice;
        d.info.connected = true;
        size = 0;
        GetRawInputDeviceInfoW(item.hDevice, RIDI_DEVICENAME, nullptr, &size);
        std::wstring path(size, L'\0');
        if (GetRawInputDeviceInfoW(item.hDevice, RIDI_DEVICENAME, path.data(), &size) == UINT(-1))
            continue;
        path.resize(wcsnlen_s(path.data(), path.size()));
        auto &identity = d.info.identity;
        identity.instance_path = utf8(path);
        if (std::any_of(existing.begin(), existing.end(), [&](const auto &known) {
                return known.connected && known.identity.instance_path == identity.instance_path;
            }))
            continue;
        d.info.usb = inspect_usb_input(path);
        if (!d.info.usb.usb)
            continue;
        identity.container_id = container_id(path);
        identity.vendor = static_cast<std::uint16_t>(rawinfo.hid.dwVendorId);
        identity.product = static_cast<std::uint16_t>(rawinfo.hid.dwProductId);
        identity.revision = static_cast<std::uint16_t>(rawinfo.hid.dwVersionNumber);
        identity.usage_page = rawinfo.hid.usUsagePage;
        identity.usage = rawinfo.hid.usUsage;
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const auto col = lower.find(L"&col");
        identity.collection_identity = col == lower.npos
                                           ? std::to_string(identity.usage_page) + ":" + std::to_string(identity.usage)
                                           : utf8(lower.substr(col, 6));
        HANDLE file =
            CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        wchar_t text[256]{};
        if (file != INVALID_HANDLE_VALUE) {
            if (HidD_GetSerialNumberString(file, text, sizeof(text)))
                identity.serial = utf8(text);
            if (HidD_GetProductString(file, text, sizeof(text)))
                identity.name = utf8(text);
            CloseHandle(file);
        }
        if (identity.name.empty())
            identity.name = "HID " + std::to_string(identity.vendor) + ":" + std::to_string(identity.product);
        size = 0;
        GetRawInputDeviceInfoW(item.hDevice, RIDI_PREPARSEDDATA, nullptr, &size);
        d.preparsed.resize(size);
        if (!size || GetRawInputDeviceInfoW(item.hDevice, RIDI_PREPARSEDDATA, d.preparsed.data(), &size) == UINT(-1))
            continue;
        auto pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data());
        HIDP_CAPS caps{};
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS)
            continue;
        d.info.report_bytes = caps.InputReportByteLength;
        bool unsupported = caps.NumberInputDataIndices > 4096 || !caps.InputReportByteLength ||
                           caps.InputReportByteLength > raw_.size();
        std::vector<HIDP_BUTTON_CAPS> buttons(caps.NumberInputButtonCaps);
        USHORT n = caps.NumberInputButtonCaps;
        if (n && HidP_GetButtonCaps(HidP_Input, buttons.data(), &n, pp) != HIDP_STATUS_SUCCESS)
            unsupported = true;
        for (const auto &b : buttons) {
            const auto first = b.IsRange ? b.Range.DataIndexMin : b.NotRange.DataIndex;
            const auto last = b.IsRange ? b.Range.DataIndexMax : b.NotRange.DataIndex;
            for (unsigned i = first; i <= last; ++i) {
                ControlInfo c;
                c.key.kind = ControlKind::button;
                c.key.usage_page = b.UsagePage;
                c.key.usage = static_cast<std::uint16_t>((b.IsRange ? b.Range.UsageMin : b.NotRange.Usage) + i - first);
                c.key.link_collection = b.LinkCollection;
                c.key.report_id = b.ReportID;
                c.key.data_index = static_cast<std::uint16_t>(i);
                c.minimum = 0;
                c.maximum = 1;
                c.bits = 1;
                c.name = control_name(c);
                d.info.controls.push_back(c);
            }
        }
        std::vector<HIDP_VALUE_CAPS> axes(caps.NumberInputValueCaps);
        n = caps.NumberInputValueCaps;
        if (n && HidP_GetValueCaps(HidP_Input, axes.data(), &n, pp) != HIDP_STATUS_SUCCESS)
            unsupported = true;
        for (const auto &a : axes) {
            if (a.ReportCount > 1 && !a.IsRange) {
                unsupported = true;
                continue;
            }
            const auto first = a.IsRange ? a.Range.DataIndexMin : a.NotRange.DataIndex;
            const auto last = a.IsRange ? a.Range.DataIndexMax : a.NotRange.DataIndex;
            for (unsigned i = first; i <= last; ++i) {
                ControlInfo c;
                c.key.kind = a.UsagePage == 1 && a.NotRange.Usage == 0x39 ? ControlKind::hat : ControlKind::axis;
                c.key.usage_page = a.UsagePage;
                c.key.usage = static_cast<std::uint16_t>((a.IsRange ? a.Range.UsageMin : a.NotRange.Usage) + i - first);
                if (a.UsagePage == 1 && c.key.usage == 0x39)
                    c.key.kind = ControlKind::hat;
                c.key.link_collection = a.LinkCollection;
                c.key.report_id = a.ReportID;
                c.key.data_index = static_cast<std::uint16_t>(i);
                c.minimum = a.LogicalMin;
                c.maximum = a.LogicalMax;
                c.bits = a.BitSize;
                c.relative = !a.IsAbsolute;
                c.name = control_name(c);
                d.info.controls.push_back(c);
            }
        }
        if (d.info.controls.size() > max_usb_controls)
            unsupported = true;
        identity.descriptor_signature = hash_descriptor(d.info.controls, caps);
        windows_names(d.info, path);
        d.info.status = unsupported ? "Unsupported descriptor: exceeds bounds or contains value arrays"
                                    : "Available; no mappings until assigned";
        if (unsupported) {
            d.info.controls.clear();
            ++rejected;
        }
        if (discovered.size() < max_usb_devices)
            discovered.push_back(std::move(d));
        else
            ++rejected;
    }
    {
        std::lock_guard<std::mutex> lock(discovery_mutex_);
        discovered_ = std::move(discovered);
    }
    SetEvent(discovered_event_);
}
void HidInput::discovery_run() {
    HANDLE events[]{stop_event_, discover_event_};
    DWORD delay = INFINITE;
    unsigned retry = 0;
    while (!stop_) {
        const DWORD wake = WaitForMultipleObjects(2, events, FALSE, delay);
        if (wake == WAIT_OBJECT_0 || wake == WAIT_FAILED)
            break;
        if (wake == WAIT_OBJECT_0 + 1)
            retry = 0;
        else
            ++retry;
        discover();
        // A device's collection/path may not yet be usable at the arrival
        // notification. Retry metadata twice, independently of input cadence.
        delay = retry < 2 ? (retry == 0 ? 250 : 1000) : INFINITE;
    }
}
void HidInput::adopt_discovered() {
    bool changed = false;
    std::vector<Cached> discovered;
    {
        std::lock_guard<std::mutex> lock(discovery_mutex_);
        discovered.swap(discovered_);
    }
    for (auto &d : discovered) {
        // A device can disappear while metadata is being read. Verify the exact
        // current path before opening it; ignore duplicate arrival notifications.
        if (std::any_of(devices_.begin(), devices_.end(), [&](const auto &known) { return known.handle == d.handle; }))
            continue;
        UINT size{};
        GetRawInputDeviceInfoW(d.handle, RIDI_DEVICENAME, nullptr, &size);
        if (!size)
            continue;
        std::wstring path(size, L'\0');
        if (GetRawInputDeviceInfoW(d.handle, RIDI_DEVICENAME, path.data(), &size) == UINT(-1))
            continue;
        path.resize(wcsnlen_s(path.data(), path.size()));
        if (utf8(path) != d.info.identity.instance_path)
            continue;
        auto slot = std::find_if(devices_.begin(), devices_.end(), [](const auto &known) { return !known.handle; });
        if (slot == devices_.end()) {
            ++rejected;
            continue;
        }
        d.info.slot = static_cast<std::uint16_t>(slot - devices_.begin());
        d.info.generation = ++generation_;
        auto &activity = activity_[d.info.slot];
        activity.reports = activity.packets = 0;
        activity.last_qpc = 0;
        *slot = std::move(d);
        changed = true;
        if (!slot->info.controls.empty())
            open_direct(*slot);
    }
    if (changed || !metadata_revision)
        publish_metadata();
}
void HidInput::open_direct(Cached &d) {
    HANDLE file =
        CreateFileW(wide(d.info.identity.instance_path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fallback(d, GetLastError());
        return;
    }
    ULONG buffers{};
    if (HidD_GetNumInputBuffers(file, &buffers))
        d.info.input_buffers = buffers;
    d.stream = std::make_unique<AsyncInput>();
    const DWORD error = d.stream->attach(file, d.info.report_bytes);
    if (error) {
        fallback(d, error);
        return;
    }
    d.info.direct = true;
    d.info.status = "Direct HID: continuous asynchronous reads; native resolution; no application rate cap";
}
void HidInput::fallback(Cached &d, DWORD error) {
    d.stream.reset();
    d.info.direct = false;
    d.raw_ready = false;
    d.known.fill(false);
    d.info.generation = ++generation_;
    d.info.status = "Raw Input fallback: direct HID error " + std::to_string(error) + "; native resolution";
    // Ignore Raw Input already queued before this backend boundary. Only fresh
    // post-barrier reports can establish the new baseline (no old button edges).
    PostMessageW(window_, fallback_ready_message, d.info.slot, static_cast<LPARAM>(d.info.generation));
}
void HidInput::complete_direct(Cached &d) {
    // Bound each device's burst; other devices, stop and hotplug get a turn.
    for (unsigned n = 0; n < 32 && d.stream && !stop_; ++n) {
        if (WaitForSingleObject(d.stream->event(), 0) != WAIT_OBJECT_0)
            break;
        const auto stamp = psx::diagnostics::qpc_now();
        const std::uint8_t *data{};
        DWORD bytes{};
        DWORD error = d.stream->complete(data, bytes);
        if (!error) {
            const DWORD rearm = d.stream->arm();
            if (!report(d, data, bytes, stamp))
                error = ERROR_INVALID_DATA;
            else
                error = rearm;
        }
        if (error) {
            fallback(d, error);
            publish_metadata();
            break;
        }
        SetEvent(available_event);
    }
}
void HidInput::raw(HRAWINPUT input) {
    const auto stamp = psx::diagnostics::qpc_now();
    UINT bytes = static_cast<UINT>(raw_.size());
    if (GetRawInputData(input, RID_INPUT, raw_.data(), &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1)) {
        ++rejected;
        return;
    }
    consume(reinterpret_cast<const RAWINPUT *>(raw_.data()), bytes, stamp);
    SetEvent(available_event);
    drain();
}
void HidInput::drain() {
    // Read the dispatched HRAWINPUT first. Buffer reads remove only subsequent
    // queued WM_INPUT packets. Bound each drain so stop/hotplug stays responsive.
    for (unsigned batch = 0; batch < 8 && !stop_; ++batch) {
        MSG device_change{};
        if (PeekMessageW(&device_change, window_, WM_INPUT_DEVICE_CHANGE, WM_INPUT_DEVICE_CHANGE, PM_NOREMOVE))
            break;
        if (PeekMessageW(&device_change, window_, fallback_ready_message, fallback_ready_message, PM_NOREMOVE))
            break;
        UINT bytes = static_cast<UINT>(raw_.size());
        const UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(raw_.data()), &bytes, sizeof(RAWINPUTHEADER));
        ++buffer_reads;
        if (!count)
            break;
        if (count == UINT(-1)) {
            ++rejected;
            break; // The ordinary path will handle/explicitly reject the next packet.
        }
        const auto stamp = psx::diagnostics::qpc_now();
        std::size_t offset = 0;
        for (UINT i = 0; i < count; ++i) {
            if (offset > raw_.size() || raw_.size() - offset < sizeof(RAWINPUTHEADER)) {
                ++rejected;
                return;
            }
            const auto packet = reinterpret_cast<const RAWINPUT *>(raw_.data() + offset);
            if (!raw_block_size(raw_.size() - offset, packet->header.dwSize)) {
                ++rejected;
                return;
            }
            consume(packet, packet->header.dwSize, stamp);
            ++buffered_packets;
            // Native x64 NEXTRAWINPUTBLOCK alignment (SDK macro requires an
            // application-defined QWORD typedef). This executable is not WOW64.
            offset += (static_cast<std::size_t>(packet->header.dwSize) + sizeof(std::uint64_t) - 1) &
                      ~(sizeof(std::uint64_t) - 1);
        }
        SetEvent(available_event);
    }
}
void HidInput::consume(const RAWINPUT *raw_input, std::size_t bytes, std::int64_t stamp) {
    constexpr std::size_t hid_offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
    if (bytes < sizeof(RAWINPUTHEADER) || !raw_block_size(bytes, raw_input->header.dwSize)) {
        ++rejected;
        return;
    }
    bytes = raw_input->header.dwSize;
    if (raw_input->header.dwType != RIM_TYPEHID)
        return;
    if (bytes < hid_offset) {
        ++rejected;
        return;
    }
    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [&](const auto &d) { return d.handle == raw_input->header.hDevice; });
    if (it == devices_.end() || it->info.controls.empty() || it->info.direct || !it->raw_ready)
        return;
    auto &d = *it;
    const auto &hid = raw_input->data.hid;
    const auto offset = hid_offset;
    if (bytes < offset || !report_batch(bytes - offset, hid.dwSizeHid, hid.dwCount)) {
        ++rejected;
        return;
    }
    for (DWORD report = 0; report < hid.dwCount; ++report) {
        this->report(d, hid.bRawData + static_cast<std::size_t>(report) * hid.dwSizeHid, hid.dwSizeHid, stamp);
    }
    ++activity_[d.info.slot].packets;
}
bool HidInput::report(Cached &d, const std::uint8_t *data, DWORD bytes, std::int64_t stamp) {
    ULONG count = static_cast<ULONG>(parsed_.size());
    if (!bytes ||
        HidP_GetData(HidP_Input, parsed_.data(), &count, reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data()),
                     reinterpret_cast<PCHAR>(const_cast<std::uint8_t *>(data)), bytes) != HIDP_STATUS_SUCCESS) {
        ++rejected;
        return false;
    }
    for (std::size_t i = 0; i < count; ++i)
        values_[i] = {parsed_[i].DataIndex, parsed_[i].RawValue};
    decoded(d, values_.data(), count, data[0], stamp);
    if (d.info.direct)
        ++activity_[d.info.slot].packets;
    return true;
}
void HidInput::decoded(Cached &d, const ReportValue *values, std::size_t count, std::uint8_t id, std::int64_t stamp) {
    report_revision_[d.info.slot].fetch_add(1,std::memory_order_acq_rel);edges.begin_batch();
    std::array<int, max_controls> decoded_values{};
    std::array<bool, max_controls> present{};
    decode_report(d.info.controls, id, values, count, decoded_values, present);
    ++reports;
    ++activity_[d.info.slot].reports;
    activity_[d.info.slot].last_qpc = stamp;
    bool has_edges = false;
    for (std::size_t i = 0; i < d.info.controls.size(); ++i) {
        if (!present[i])
            continue;
        const auto &c = d.info.controls[i];
        const bool baseline = !d.known[i];
        const bool changed = baseline || decoded_values[i] != d.values[i] || c.relative;
        Sample sample{d.info.slot,
                      static_cast<std::uint16_t>(i),
                      d.info.generation,
                      ++sequence_,
                      stamp,
                      decoded_values[i],
                      baseline,
                      c.relative};
        latest_[d.info.slot][i].store(sample);
        if (changed && (c.key.kind != ControlKind::axis || c.relative)) {
            has_edges = true;
            if (!edges.push(sample))
                ++overflow;
        }
        d.known[i] = true;
        d.values[i] = decoded_values[i];
    }
    if (has_edges) {
        Sample boundary;
        boundary.device = d.info.slot;
        boundary.generation = d.info.generation;
        boundary.qpc = stamp;
        boundary.report_end = true;
        if (!edges.push(boundary))
            ++overflow;
    }
    edges.commit_batch();report_revision_[d.info.slot].fetch_add(1,std::memory_order_release);
}
LRESULT CALLBACK HidInput::window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<HidInput *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<HidInput *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        if (message == WM_INPUT) {
            self->raw(reinterpret_cast<HRAWINPUT>(l));
            return DefWindowProcW(hwnd, message, w, l);
        }
        if (message == WM_INPUT_DEVICE_CHANGE) {
            if (w == GIDC_REMOVAL)
                self->remove(reinterpret_cast<HANDLE>(l));
            else
                SetEvent(self->discover_event_);
            return 0;
        }
        if (message == fallback_ready_message) {
            if (w < max_usb_devices) {
                auto &d = self->devices_[w];
                if (d.info.connected && !d.info.direct && d.info.generation == static_cast<std::uint64_t>(l)) {
                    // Posted messages can be retrieved ahead of input messages.
                    // Explicitly discard this collection's queued Raw Input while
                    // the gate is still closed; do not rely on posting order.
                    MSG queued{};
                    for (unsigned n = 0; n < 32 && PeekMessageW(&queued, hwnd, WM_INPUT, WM_INPUT, PM_REMOVE); ++n)
                        DispatchMessageW(&queued);
                    if (PeekMessageW(&queued, hwnd, WM_INPUT, WM_INPUT, PM_NOREMOVE))
                        PostMessageW(hwnd, message, w, l);
                    else
                        d.raw_ready = true;
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, w, l);
}
void HidInput::run() {
    thread_id_ = GetCurrentThreadId();
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PSX_HWIO_INPUT";
    RegisterClassW(&wc);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!window_) {
        ++rejected;
        thread_id_ = 0;
        return;
    }
    RAWINPUTDEVICE registrations[]{{1, 4, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_},
                                   {1, 8, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_},
                                   {2, 0, RIDEV_PAGEONLY | RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_}};
    if (!RegisterRawInputDevices(registrations, static_cast<UINT>(std::size(registrations)), sizeof(RAWINPUTDEVICE)))
        ++rejected;
    discovery_thread_ = std::thread(&HidInput::discovery_run, this);
    SetEvent(discover_event_);
    while (!stop_) {
        // Process lifecycle messages first, with a bound so Raw Input cannot
        // starve direct completions. Duplicate Raw Input reports are discarded.
        MSG msg{};
        for (unsigned n = 0; n < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (msg.message == WM_QUIT) {
                stop_ = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (WaitForSingleObject(discovered_event_, 0) == WAIT_OBJECT_0)
            adopt_discovered();
        for (auto &d : devices_)
            if (d.stream)
                complete_direct(d);
        HANDLE events[max_usb_devices + 2]{stop_event_, discovered_event_};
        DWORD count = 2;
        for (const auto &d : devices_)
            if (d.stream)
                events[count++] = d.stream->event();
        if (stop_)
            break;
        const DWORD wake = MsgWaitForMultipleObjectsEx(count, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wake == WAIT_FAILED) {
            ++rejected;
            break;
        }
        // Auto-reset metadata event may have been consumed by the wait.
        if (wake == WAIT_OBJECT_0 + 1)
            adopt_discovered();
    }
    stop_ = true;
    SetEvent(stop_event_);
    if (discovery_thread_.joinable())
        discovery_thread_.join();
    for (auto &d : devices_) {
        d.stream.reset();
        if (d.handle) {
            d.handle = nullptr;
            d.info.connected = d.info.direct = d.raw_ready = false;
            d.info.generation = ++generation_;
            d.info.status = "Input reader stopped";
            d.known.fill(false);
        }
    }
    publish_metadata();
    DestroyWindow(window_);
    window_ = nullptr;
    thread_id_ = 0;
}
} // namespace hwio
