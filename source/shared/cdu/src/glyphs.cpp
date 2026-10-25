#include "psx/cdu/glyphs.hpp"
#include <algorithm>
#include <cmath>
namespace psx::cdu
{
namespace
{
struct Author
{
    Glyph glyph;
    float left, right, top, bottom, middle;
    explicit Author(char c, Size s) : glyph{c, s}
    {
        // Small profile has its own metrics/origin and distinct details below.
        left = s == Size::large ? -5.336f : -5.5f;
        right = s == Size::large ? 5.336f : 2.5f;
        top = s == Size::large ? -7.85f : -4.1625f;
        bottom = s == Size::large ? 7.85f : 7.6125f;
        middle = (top + bottom) / 2;
    }
    ag_point p(float x, float y)
    {
        return {left + (right - left) * x, top + (bottom - top) * y};
    }
    void move(float x, float y)
    {
        glyph.contours.push_back({AG_PATH_MOVE, p(x, y)});
    }
    void line(float x, float y)
    {
        glyph.contours.push_back({AG_PATH_LINE, p(x, y)});
    }
    void q(float x, float y, float ex, float ey)
    {
        glyph.contours.push_back({AG_PATH_QUADRATIC, p(x, y), p(ex, ey)});
    }
    void close()
    {
        glyph.contours.push_back({AG_PATH_CLOSE});
    }
    void segment(float x, float y, float ex, float ey)
    {
        move(x, y);
        line(ex, ey);
    }
    void oval(float x0, float y0, float x1, float y1)
    {
        float xm = (x0 + x1) / 2, h = y1 - y0;
        move(x0, y0 + h * .446f);
        q(x0, y0, xm, y0);
        q(x1, y0, x1, y0 + h * .446f);
        line(x1, y0 + h * .554f);
        q(x1, y1, xm, y1);
        q(x0, y1, x0, y0 + h * .554f);
        close();
    }
    Glyph finish()
    {
        ag_rect b{1e6f, 1e6f, -1e6f, -1e6f};
        for (auto &c : glyph.contours)
        {
            unsigned n = c.kind == AG_PATH_QUADRATIC ? 2 : c.kind == AG_PATH_CLOSE ? 0 : 1;
            ag_point pts[]{c.p1, c.p2};
            for (unsigned i = 0; i < n; ++i)
            {
                b.left = std::min(b.left, pts[i].x);
                b.right = std::max(b.right, pts[i].x);
                b.top = std::min(b.top, pts[i].y);
                b.bottom = std::max(b.bottom, pts[i].y);
            }
        }
        if (glyph.contours.empty())
            b = {};
        glyph.bounds = b;
        return std::move(glyph);
    }
};
ag_rect metrics(char c, Size s)
{
    bool big = s == Size::large;
    float x = big ? -5.336f : -5.5f, y = big ? -7.85f : -4.1625f, w = big ? 10.672f : 8,
          h = big ? 15.7f : 11.775f;
    // Independently measured path bounds from the installed reference sheet,
    // rounded to 0.0001 logical unit. Shapes below remain authored contours.
    switch (c)
    {
    case 'I':
        x = big ? 0 : -2.5f;
        w = 0;
        break;
    case '1':
        x = big ? -3.0015f : -3.75f;
        w = big ? 6.003f : 4.5f;
        break;
    case '<':
        x = big ? -4.002f : -4.5f;
        y = big ? -4.5f : -1.65f;
        w = big ? 6.003f : 4.5f;
        h = big ? 9 : 6.75f;
        break;
    case '>':
        x = big ? -2.001f : -3;
        y = big ? -4.5f : -1.65f;
        w = big ? 6.003f : 4.5f;
        h = big ? 9 : 6.75f;
        break;
    case '.':
        x = big ? -3.6018f : -4.2f;
        y = big ? 4.0687f : 5.35f;
        w = h = big ? 3.2016f : 2.4f;
        break;
    case ',':
        x = big ? -2.668f : -3.5f;
        y = big ? 4.3355f : 5.55f;
        w = big ? 1.334f : 1;
        h = big ? 3.6685f : 2.75f;
        break;
    case ':':
        x = big ? -1.6008f : -2.7f;
        y = big ? -1.9343f : .85f;
        w = big ? 3.2016f : 2.4f;
        h = big ? 9.2046f : 6.9f;
        break;
    case '%':
        y = big ? -7.8706f : -3.6f;
        h = big ? 15.7412f : 11.8f;
        break;
    case '+':
    case '#':
        x = big ? -4.669f : -5;
        y = big ? -4.669f : -1.2f;
        w = h = big ? 9.338f : 7;
        break;
    case '-':
        x = big ? -4.669f : -5;
        y = big ? 0 : 2.3f;
        w = big ? 9.338f : 7;
        h = 0;
        break;
    case '=':
        x = big ? -4.669f : -5;
        y = big ? -2.3345f : .55f;
        w = big ? 9.338f : 7;
        h = big ? 4.669f : 3.5f;
        break;
    case '*':
        x = big ? -4.669f : -5;
        y = big ? -8.004f : -3.7f;
        w = h = big ? 9.338f : 7;
        break;
    case '(':
        x = big ? -3.335f : -4;
        y = big ? -8.3375f : -3.95f;
        w = big ? 3.335f : 2.5f;
        h = big ? 16.675f : 12.5f;
        break;
    case ')':
        x = big ? 0 : -1.5f;
        y = big ? -8.3375f : -3.95f;
        w = big ? 3.335f : 2.5f;
        h = big ? 16.675f : 12.5f;
        break;
    case 'o':
        x = big ? -1.334f : -2.5f;
        y = big ? -8.004f : -3.7f;
        w = h = big ? 5.336f : 4;
        break;
    case 'b':
        x = big ? -7.337f : -7;
        w = big ? 14.674f : 11;
        break;
    case 'u':
    case 'd':
        x = big ? -4.669f : -5;
        w = big ? 9.338f : 7;
        break;
    case 'l':
        y = big ? -4.75f : -1.8375f;
        h = big ? 9.5f : 7.125f;
        break;
    case 'r':
        x = big ? -4.8691f : -5.15f;
        w = big ? 10.2051f : 7.65f;
        y = big ? -4.75f : -1.8375f;
        h = big ? 9.5f : 7.125f;
        break;
    case 't':
        x = big ? -5.0025f : -5.25f;
        w = big ? 10.005f : 7.5f;
        y = big ? .5f : 2.1f;
        h = big ? 7.35f : 5.5125f;
        break;
    }
    return {x, y, x + w, y + h};
}
Glyph author(char c, Size s)
{
    Author a(c, s);
    switch (c)
    {
    case 'A':
        a.move(0, 1);
        a.line(.5f, 0);
        a.line(1, 1);
        a.segment(.28f, .69f, .72f, .69f);
        break;
    case 'B':
        a.move(0, 1);
        a.line(0, 0);
        a.line(.469f, 0);
        a.q(.894f, 0, .969f, .197f);
        a.q(1, .452f, .438f, .468f);
        a.line(.062f, .468f);
        a.line(.5f, .468f);
        a.q(1, .516f, 1, .723f);
        a.q(1, .984f, .5f, 1);
        a.close();
        break;
    case 'C':
        a.move(1, .16f);
        a.q(.875f, 0, .5f, 0);
        a.q(0, .064f, 0, .318f);
        a.line(0, .682f);
        a.q(0, .936f, .5f, 1);
        a.q(.875f, 1, 1, .84f);
        break;
    case 'D':
        a.move(0, 1);
        a.line(0, 0);
        a.line(.375f, 0);
        a.q(1, 0, 1, .436f);
        a.line(1, .564f);
        a.q(1, 1, .375f, 1);
        a.close();
        break;
    case 'E':
        a.move(1, 0);
        a.line(0, 0);
        a.line(0, 1);
        a.line(1, 1);
        a.segment(.813f, .468f, .062f, .468f);
        break;
    case 'F':
        a.move(1, 0);
        a.line(0, 0);
        a.line(0, 1);
        a.segment(.813f, .468f, .062f, .468f);
        break;
    case 'G':
        a.move(1, .22f);
        a.q(.906f, 0, .5f, 0);
        a.q(.094f, 0, 0, .287f);
        a.line(0, .713f);
        a.q(.094f, 1, .5f, 1);
        a.q(.906f, 1, 1, .713f);
        a.line(1, .564f);
        a.line(.594f, .564f);
        break;
    case 'H':
        a.segment(0, 0, 0, 1);
        a.segment(1, 0, 1, 1);
        a.segment(.062f, .484f, .938f, .484f);
        break;
    case 'I':
        a.segment(s == Size::large ? .5f : .375f, 0, s == Size::large ? .5f : .375f, 1);
        break;
    case 'J':
        a.move(1, 0);
        a.line(1, .745f);
        a.q(.938f, 1, .5f, 1);
        a.q(.125f, 1, 0, .793f);
        break;
    case 'K':
        a.segment(0, 0, 0, 1);
        a.segment(1, 0, .062f, .484f);
        a.segment(.219f, .468f, 1, 1);
        break;
    case 'L':
        a.move(0, 0);
        a.line(0, 1);
        a.line(1, 1);
        break;
    case 'M':
        a.move(0, 1);
        a.line(0, 0);
        a.line(.5f, .436f);
        a.line(1, 0);
        a.line(1, 1);
        break;
    case 'N':
        a.move(0, 1);
        a.line(0, 0);
        a.line(1, 1);
        a.line(1, 0);
        break;
    case 'O':
        a.oval(0, 0, 1, 1);
        break;
    case 'P':
        a.move(0, 1);
        a.line(0, 0);
        a.line(.406f, 0);
        a.q(.969f, 0, 1, .245f);
        a.q(1, .484f, .406f, .5f);
        a.line(.062f, .5f);
        break;
    case 'Q':
        a.oval(0, 0, 1, 1);
        a.segment(.563f, .659f, 1, 1);
        break;
    case 'R':
        a.move(0, 1);
        a.line(0, 0);
        a.line(.406f, 0);
        a.q(.969f, 0, 1, .245f);
        a.q(1, .484f, .406f, .5f);
        a.line(.062f, .5f);
        a.segment(.563f, .532f, 1, 1);
        break;
    case 'S':
        a.move(.938f, .127f);
        a.q(.762f, 0, .469f, 0);
        a.q(.05f, .016f, 0, .223f);
        a.q(0, .404f, .5f, .478f);
        a.q(1, .548f, 1, .745f);
        a.q(1, .978f, .5f, 1);
        a.q(.156f, 1, 0, .841f);
        break;
    case '5':
        a.move(.938f, 0);
        a.line(0, 0);
        a.line(0, .452f);
        a.line(.5f, .452f);
        a.q(1, .452f, 1, .768f);
        a.q(1, 1, .5f, 1);
        a.q(.075f, 1, 0, .866f);
        break;
    case 'T':
        a.segment(0, 0, 1, 0);
        a.segment(.5f, .032f, .5f, 1);
        break;
    case 'U':
        a.move(0, 0);
        a.line(0, .554f);
        a.q(0, 1, .5f, 1);
        a.q(1, 1, 1, .554f);
        a.line(1, 0);
        break;
    case 'V':
        a.move(0, 0);
        a.line(.5f, 1);
        a.line(1, 0);
        break;
    case 'W':
        a.move(0, 0);
        a.line(0, 1);
        a.line(.5f, .564f);
        a.line(1, 1);
        a.line(1, 0);
        break;
    case 'X':
        a.segment(0, 0, 1, 1);
        a.segment(1, 0, 0, 1);
        break;
    case 'Y':
        a.move(0, 0);
        a.line(.5f, .5f);
        a.line(1, 0);
        a.segment(.5f, .532f, .5f, 1);
        break;
    case 'Z':
        a.move(0, 0);
        a.line(1, 0);
        a.line(0, 1);
        a.line(1, 1);
        break;
    case '0':
        a.oval(0, 0, 1, 1);
        a.segment(1, 0, 0, 1);
        break;
    case '1':
        a.move(.067f, .134f);
        a.line(.5f, 0);
        a.line(.5f, 1);
        a.segment(0, 1, 1, 1);
        break;
    case '2':
        a.move(0, .156f);
        a.q(.231f, 0, .469f, 0);
        a.q(.906f, 0, .969f, .169f);
        a.q(1, .28f, .725f, .462f);
        a.line(0, .943f);
        a.line(0, 1);
        a.line(1, 1);
        break;
    case '3':
        a.move(.037f, .121f);
        a.q(.244f, 0, .5f, 0);
        a.q(.913f, 0, .981f, .172f);
        a.q(1, .328f, .531f, .462f);
        a.q(1, .541f, 1, .758f);
        a.q(.969f, .984f, .5f, 1);
        a.q(.181f, 1, 0, .869f);
        break;
    case '4':
        a.move(.738f, 1);
        a.line(.738f, 0);
        a.line(0, .71f);
        a.line(1, .71f);
        break;
    case '6':
        a.move(.813f, 0);
        a.line(.5f, 0);
        a.q(0, 0, 0, .255f);
        a.line(0, .745f);
        a.q(0, 1, .5f, 1);
        a.q(1, 1, 1, .745f);
        a.q(1, .5f, .625f, .5f);
        a.q(0, .5f, .025f, .697f);
        break;
    case '7':
        a.move(0, 0);
        a.line(1, 0);
        a.line(.0625f, 1);
        break;
    case '8':
        a.move(.031f, .207f);
        a.q(.081f, .01f, .5f, 0);
        a.q(.913f, .006f, .969f, .207f);
        a.q(.969f, .373f, .5f, .468f);
        a.q(0, .58f, 0, .777f);
        a.q(.062f, .968f, .5f, 1);
        a.q(.938f, .968f, 1, .777f);
        a.q(1, .548f, .5f, .468f);
        a.q(.031f, .404f, .031f, .207f);
        a.close();
        break;
    case '9':
        a.move(.094f, 1);
        a.line(.5f, 1);
        a.q(1, 1, 1, .745f);
        a.line(1, .255f);
        a.q(1, 0, .5f, 0);
        a.q(0, 0, 0, .255f);
        a.q(0, .5f, .375f, .5f);
        a.q(1, .5f, .975f, .303f);
        break;
    case '<':
    case '>': {
        float x = c == '<' ? 0 : 1;
        a.move(1 - x, .32f);
        a.line(x, .5f);
        a.line(1 - x, .68f);
        break;
    }
    case '.':
        a.move(.5f, .8f);
        a.line(.6f, .9f);
        a.line(.5f, 1);
        a.line(.4f, .9f);
        a.close();
        break;
    case ',':
        a.segment(.5f, .9f, .4f, 1.1f);
        break;
    case ':':
        for (float y : {.174f, .826f})
        {
            a.move(0, y);
            a.line(.5f, y - .174f);
            a.line(1, y);
            a.line(.5f, y + .174f);
            a.close();
        }
        break;
    case '/':
        a.segment(0, 1, 1, 0);
        break;
    case '%':
        a.segment(0, 1, 1, 0);
        for (float x : {.156f, .844f})
        {
            float y = x < .5f ? .14f : .86f;
            a.move(x - .15f, y);
            a.line(x, y - .102f);
            a.line(x + .15f, y);
            a.line(x, y + .102f);
            a.close();
        }
        break;
    case '+':
        a.segment(.15f, .5f, .85f, .5f);
        a.segment(.5f, .28f, .5f, .72f);
        break;
    case '-':
        a.segment(.15f, .5f, .85f, .5f);
        break;
    case '=':
        a.segment(.15f, .45f, .85f, .45f);
        a.segment(.15f, .62f, .85f, .62f);
        break;
    case '*':
        a.segment(.5f, 0, .5f, 1);
        a.segment(0, .5f, 1, .5f);
        a.segment(.15f, .15f, .85f, .85f);
        a.segment(.85f, .15f, .15f, .85f);
        break;
    case '(':
    case ')': {
        bool left = c == '(';
        float x = left ? 1 : 0, z = 1 - x;
        a.move(x, 0);
        a.line(z, .24f);
        a.line(z, .76f);
        a.line(x, 1);
        break;
    }
    case '#':
        a.segment(3.f / 14, 0, 3.f / 14, 1);
        a.segment(11.f / 14, 0, 11.f / 14, 1);
        a.segment(0, 3.f / 14, 1, 3.f / 14);
        a.segment(0, 11.f / 14, 1, 11.f / 14);
        break;
    case 'o':
        a.move(.375f, 0);
        a.line(.625f, .125f);
        a.line(.375f, .25f);
        a.line(.125f, .125f);
        a.close();
        break;
    case 'b':
        a.move(-.1875f, 0);
        a.line(1.1875f, 0);
        a.line(1.1875f, 1);
        a.line(-.1875f, 1);
        a.close();
        break;
    case 'u':
    case 'd': {
        bool up = c == 'u';
        float y = up ? 0 : 1;
        a.move(0, up ? .341f : .659f);
        a.line(.5f, y);
        a.line(1, up ? .341f : .659f);
        a.segment(.5f, up ? .022f : 0, .5f, up ? 1 : .978f);
        break;
    }
    case 'l':
    case 'r': {
        float x = c == 'l' ? 0 : 1, head = c == 'l' ? .5f : .477f;
        a.segment(c == 'l' ? .044f : 0, .5f, 1, .5f);
        a.move(head, 1);
        a.line(x, .5f);
        a.line(head, 0);
        break;
    }
    case 't':
        a.move(.03f, 1);
        a.line(.5f, .53f);
        a.line(.97f, 1);
        a.close();
        break;
    default:
        break;
    }
    auto g = a.finish();
    if (g.contours.empty())
        return g;
    auto desired = metrics(c, s);
    auto before = g.bounds;
    for (auto &cmd : g.contours)
    {
        unsigned n = cmd.kind == AG_PATH_QUADRATIC ? 2 : cmd.kind == AG_PATH_CLOSE ? 0 : 1;
        ag_point *p[]{&cmd.p1, &cmd.p2};
        for (unsigned i = 0; i < n; ++i)
        {
            p[i]->x = before.right > before.left
                          ? desired.left + (p[i]->x - before.left) / (before.right - before.left) *
                                               (desired.right - desired.left)
                          : desired.left;
            p[i]->y = before.bottom > before.top
                          ? desired.top + (p[i]->y - before.top) / (before.bottom - before.top) *
                                              (desired.bottom - desired.top)
                          : desired.top;
        }
    }
    g.bounds = desired;
    return g;
}
} // namespace
const std::vector<Glyph> &glyphs()
{
    static const auto resource = [] {
        std::vector<Glyph> g;
        for (char c : glyph_codes)
            for (Size s : {Size::small, Size::large})
                g.push_back(author(c, s));
        return g;
    }();
    return resource;
}
} // namespace psx::cdu
