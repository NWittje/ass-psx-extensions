#include <algorithm>
#include <cmath>
#include <numbers>
#include <psx/display/replay.hpp>
#include <psx/display/instrument_protocol.hpp>
namespace psx::du
{
void Resource::bind()
{
    descriptor.segments = segments.data();
    descriptor.segment_count = uint32_t(segments.size());
    descriptor.dashes = dashes.data();
    descriptor.dash_count = uint32_t(dashes.size());
    descriptor.pixels = pixels.data();
    descriptor.pixel_bytes = uint32_t(pixels.size());
}
size_t Resource::bytes() const
{
    return sizeof(*this) + segments.capacity() * sizeof(ag_replay_segment) + dashes.capacity() * 4 +
           pixels.capacity();
}
size_t Frame::bytes() const
{
    size_t n = sizeof(*this) + commands.capacity() * sizeof(ag_replay_command) +
               descriptors.capacity() * sizeof(ag_replay_resource) + resources.capacity() * sizeof(Resource);
    for (auto &r : resources)
        n += r.bytes() - sizeof(r);
    return n;
}
ag_replay_snapshot Frame::snapshot() const
{
    return {sizeof(ag_replay_snapshot),
            1,
            uint32_t(commands.size()),
            uint32_t(descriptors.size()),
            commands.data(),
            descriptors.data()};
}
namespace
{
void need(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
ag_replay_command command(unsigned k, unsigned ctx)
{
    ag_replay_command c{};
    c.size = sizeof c;
    c.version = 1;
    c.kind = k;
    c.context = ctx;
    return c;
}
void segment(Resource &r, unsigned kind, std::initializer_list<double> p)
{
    ag_replay_segment s{};
    s.kind = kind;
    std::copy(p.begin(), p.end(), s.p);
    r.segments.push_back(s);
}
void ellipse(Resource &r, double x, double y, double w, double h)
{
    constexpr double k = .5522847498307933;
    auto cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    segment(r, 0, {cx + rx, cy});
    segment(r, 3, {cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry});
    segment(r, 3, {cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy});
    segment(r, 3, {cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry});
    segment(r, 3, {cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy});
    segment(r, 4, {});
}
Resource primitive(unsigned k, const std::vector<int> &a)
{
    Resource r;
    r.descriptor.kind = 1;
    r.descriptor.winding = 1;
    auto x = double(a.size() > 0 ? a[0] : 0), y = double(a.size() > 1 ? a[1] : 0),
         w = double(a.size() > 2 ? a[2] : 0), h = double(a.size() > 3 ? a[3] : 0);
    if (k == 1)
    {
        segment(r, 0, {x, y});
        segment(r, 1, {w, h});
    }
    else if (k <= 4)
    {
        if (w < 0 || h < 0)
            return r;
        segment(r, 0, {x, y});
        segment(r, 1, {x + w, y});
        segment(r, 1, {x + w, y + h});
        segment(r, 1, {x, y + h});
        segment(r, 4, {});
    }
    else if (k <= 6)
    {
        if (w <= 0 || h <= 0)
            return r;
        ellipse(r, x, y, w, h);
    }
    else if (k <= 8)
    {
        if (w <= 0 || h <= 0)
            return r;
        double rx = std::min(w, std::abs(double(a[4]))) / 2, ry = std::min(h, std::abs(double(a[5]))) / 2;
        constexpr double v = .5522847498307933;
        segment(r, 0, {x + rx, y});
        segment(r, 1, {x + w - rx, y});
        segment(r, 3, {x + w - rx + rx * v, y, x + w, y + ry - ry * v, x + w, y + ry});
        segment(r, 1, {x + w, y + h - ry});
        segment(r, 3, {x + w, y + h - ry + ry * v, x + w - rx + rx * v, y + h, x + w - rx, y + h});
        segment(r, 1, {x + rx, y + h});
        segment(r, 3, {x + rx - rx * v, y + h, x, y + h - ry + ry * v, x, y + h - ry});
        segment(r, 1, {x, y + ry});
        segment(r, 3, {x, y + ry - ry * v, x + rx - rx * v, y, x + rx, y});
        segment(r, 4, {});
    }
    else if (k <= 10)
    {
        if (w <= 0 || h <= 0 || a[5] == 0)
            return r;
        double extent = std::clamp(double(a[5]), -360., 360.), start = double(a[4]) * std::numbers::pi / 180,
               step = extent * std::numbers::pi / 180 / std::ceil(std::abs(extent) / 90);
        auto cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
        auto point = [&](double angle)
        { return std::pair{cx + rx * std::cos(angle), cy - ry * std::sin(angle)}; };
        auto p = point(start);
        if (k == 10)
        {
            segment(r, 0, {cx, cy});
            segment(r, 1, {p.first, p.second});
        }
        else
            segment(r, 0, {p.first, p.second});
        unsigned n = unsigned(std::ceil(std::abs(extent) / 90));
        for (unsigned i = 0; i < n; i++)
        {
            double end = start + step, t = 4. / 3 * std::tan(step / 4);
            auto q = point(end);
            segment(r, 3,
                    {p.first - t * rx * std::sin(start), p.second - t * ry * std::cos(start),
                     q.first + t * rx * std::sin(end), q.second + t * ry * std::cos(end), q.first, q.second});
            start = end;
            p = q;
        }
        if (k == 10)
            segment(r, 4, {});
    }
    else
    {
        if (a.empty())
            return r;
        segment(r, 0, {double(a[0]), double(a[1])});
        for (size_t i = 2; i < a.size(); i += 2)
            segment(r, 1, {double(a[i]), double(a[i + 1])});
        if (k != 11)
            segment(r, 4, {});
        r.descriptor.winding = 0;
    }
    return r;
}
} // namespace
static std::shared_ptr<const Frame> decode_model(const std::vector<uint8_t> &b, psx::recorder::Decoded header)
{
    auto f = std::make_shared<Frame>();
    f->header = header;
    f->flags = psx::recorder::integer(b.data(), 144);
    need(f->header.operations <= 65536 && f->header.resources <= 65536, "DPF1 replay count budget");
    need(f->header.kind == 1 || f->header.kind >= 4 || f->header.operations == 0, "Non-drawing transition contains commands");
    psx::recorder::Reader r(b);
    size_t at = 256;
    std::vector<uint32_t> ids(f->header.resources + 1);
    size_t payload_bytes = 0;
    f->commands.reserve(f->header.operations);
    while (at < b.size())
    {
        r.bounds(at, b.size());
        auto code = r.take<uint32_t>(), length = r.take<uint32_t>(), ctx = r.take<uint32_t>();
        r.take<uint32_t>();
        at = r.position();
        auto end = at + length;
        r.bounds(at, end);
        if (code <= 3)
        {
            auto id = r.take<uint32_t>();
            Resource resource;
            resource.descriptor.kind = code;
            if (code == 1)
            {
                auto precision = r.take<uint32_t>();
                resource.descriptor.winding = r.take<uint32_t>();
                auto n = r.take<uint32_t>();
                need(n <= 65536 && payload_bytes + size_t(n) * sizeof(ag_replay_segment) <= 8 * 1024 * 1024,
                     "Path expansion CPU budget exceeded");
                resource.segments.reserve(n);
                for (unsigned i = 0; i < n; i++)
                {
                    ag_replay_segment s{};
                    s.kind = r.take<uint32_t>();
                    unsigned v = s.kind == 4 ? 0 : s.kind == 3 ? 6 : s.kind == 2 ? 4 : 2;
                    for (unsigned j = 0; j < v; j++)
                        s.p[j] = precision == 4 ? double(r.real<float>()) : r.real<double>();
                    resource.segments.push_back(s);
                }
            }
            else if (code == 2)
            {
                auto &d = resource.descriptor;
                d.width = r.real<float>();
                d.cap = r.take<uint32_t>();
                d.join = r.take<uint32_t>();
                d.miter = r.real<float>();
                d.phase = r.real<float>();
                auto n = r.take<uint32_t>();
                for (unsigned i = 0; i < n; i++)
                    resource.dashes.push_back(r.real<float>());
                need(d.width > 0 || resource.dashes.empty(), "Unsupported dashed hairline");
            }
            else
            {
                auto &d = resource.descriptor;
                d.image_width = r.take<uint32_t>();
                d.image_height = r.take<uint32_t>();
                d.stride = d.image_width * 4;
                auto n = uint64_t(d.stride) * d.image_height;
                need(payload_bytes + n <= 8 * 1024 * 1024, "Bitmap CPU budget exceeded");
                resource.pixels.resize(size_t(n));
                for (size_t i = 0; i < n; i += 4)
                {
                    auto argb = r.take<uint32_t>();
                    auto a = argb >> 24;
                    resource.pixels[i] = uint8_t(((argb & 255) * a + 127) / 255);
                    resource.pixels[i + 1] = uint8_t((((argb >> 8) & 255) * a + 127) / 255);
                    resource.pixels[i + 2] = uint8_t((((argb >> 16) & 255) * a + 127) / 255);
                    resource.pixels[i + 3] = uint8_t(a);
                }
            }
            resource.descriptor.id = uint32_t(f->resources.size() + 1);
            ids[id] = resource.descriptor.id;
            payload_bytes += resource.bytes();
            f->resources.push_back(std::move(resource));
        }
        else
        {
            ag_replay_command c = command(0, ctx);
            if (code == 4 || code == 5 || code == 6 || code == 11 || code == 14)
            {
                c.kind = code == 4    ? AG_REPLAY_DRAW
                         : code == 5  ? AG_REPLAY_FILL
                         : code == 6  ? AG_REPLAY_CLIP
                         : code == 11 ? AG_REPLAY_USE_STROKE
                                      : AG_REPLAY_INTERSECT;
                c.resource = ids.at(r.take<uint32_t>());
            }
            else if (code == 7)
            {
                c.kind = AG_REPLAY_CREATE;
                c.value = r.take<uint32_t>();
            }
            else if (code == 8)
                c.kind = AG_REPLAY_DISPOSE;
            else if (code == 9)
            {
                c.kind = AG_REPLAY_TRANSFORM;
                for (auto &v : c.matrix)
                    v = r.real<double>();
            }
            else if (code == 10)
            {
                c.kind = AG_REPLAY_COLOUR;
                c.argb = r.take<uint32_t>();
            }
            else if (code == 12)
            {
                need(r.take<uint32_t>() == 3, "Unsupported composite: requires SRC_OVER");
                c.kind = AG_REPLAY_OPACITY;
                c.opacity = r.real<float>();
            }
            else if (code == 13)
            {
                auto key = r.take<uint32_t>(), v = r.take<uint32_t>();
                if (key == 1)
                {
                    need(v == 2, "Unsupported stroke normalization: requires STROKE_PURE");
                    at = end;
                    continue;
                }
                if (key == 0)
                {
                    need(v == 1 || v == 2, "Unsupported default antialiasing hint");
                    c.kind = AG_REPLAY_AA;
                    c.value = v == 2;
                }
                else
                {
                    need(v <= 1, "Unsupported bicubic sampling");
                    c.kind = AG_REPLAY_SAMPLING;
                    c.value = v;
                }
            }
            else if (code == 15)
            {
                auto kind = r.take<uint32_t>(), n = r.take<uint32_t>();
                std::vector<int> a;
                for (unsigned i = 0; i < n; i++)
                    a.push_back(r.take<int32_t>());
                auto res = primitive(kind, a);
                res.descriptor.id = uint32_t(f->resources.size() + 1);
                c.resource = res.descriptor.id;
                payload_bytes += res.bytes();
                f->resources.push_back(std::move(res));
                c.kind = kind == 3                                                           ? AG_REPLAY_CLEAR
                         : (kind == 2 || kind == 6 || kind == 8 || kind == 10 || kind == 13) ? AG_REPLAY_FILL
                                                                                             : AG_REPLAY_DRAW;
            }
            else if (code == 16)
            {
                c.kind = AG_REPLAY_BITMAP;
                c.resource = ids.at(r.take<uint32_t>());
                for (auto &v : c.matrix)
                    v = r.real<double>();
            }
            f->commands.push_back(c);
        }
        at = end;
        need(payload_bytes + f->commands.capacity() * sizeof(ag_replay_command) +
                     f->resources.capacity() * sizeof(Resource) +
                     f->resources.size() * sizeof(ag_replay_resource) <=
                 8 * 1024 * 1024,
             "Decoded replay CPU budget exceeded");
        need(f->resources.size() <= 65536, "Replay resource expansion budget exceeded");
    }
    f->descriptors.reserve(f->resources.size());
    for (auto &resource : f->resources)
    {
        resource.bind();
        f->descriptors.push_back(resource.descriptor);
    }
    need(f->bytes() <= 8 * 1024 * 1024, "Decoded replay CPU budget exceeded");
    return f;
}
std::shared_ptr<const Frame> decode(const std::vector<uint8_t> &b)
{
    return decode_model(b, psx::recorder::decode(b));
}
std::shared_ptr<const Frame> decode_instrument(const std::vector<uint8_t> &b)
{
    return decode_model(b, psx::instrument::decode(b));
}
} // namespace psx::du
