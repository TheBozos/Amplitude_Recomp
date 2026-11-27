// SPDX-License-Identifier: MIT
// The PC SETTINGS entry's files: the game's own menus extended at startup from the player's disc (runtime/amp_menu.h).
//
// Four files of GEN/MAIN.ARK, read from the disc, edited here and given to the archive overlay (amp_ark_overlay.h),
// which lays them over the sectors the game reads. A file keeps its place and its size in the archive (the game's
// reader goes by the offsets), so each edit stays within the original's bytes, the room made by taking out what only
// the demo disc shows. Anything unexpected (another disc, an edit that does not fit) leaves all four as they are.
//
// - metagame/gen/meta.ui.bin (the menus' definitions, a binary DataArray: version 2, the source files' names, the root
//   list; a list is a u16 count, a u32 id, a u32 file index, its elements' kinds 2 bits each in u32 words (0 int,
//   1 symbol, 2 float, 3 list), then the elements, a symbol as a u32 length and its bytes): a button
//   (button_comp pc_settings buttonStyle2 <object>) after the last of the Options panel o_main and of the pause panel
//   s_pause, and pc_settings at the end of their navigator's vertical order. The source files' names stay: the game
//   finds a screen's lists by them. Room: the demo's four error screens and dialogs.
// - gen/english_messages.txt.bin (the same format, (key "text") lists): the Options panel's help line for it,
//   o_main_pc_settings_HELP. Room: two demo-only messages.
// - metagame/o_main.rnd and metagame/s_pause.rnd (the panels' scenes: Harmonix's container (magic AFDEBECC, its
//   header's length, the blocks (1), the whole size, the block's size, 0xCC to the header's length), one gzip
//   stream, then a directory: u32 version (10), u32 count, count x (type, name), then each object's data in that order,
//   each ending with AD DE AD DE): the last button's Text and Mesh cloned a row (24 units) lower (the Mesh's local y
//   at +0x30 and world z at +0x64, the Text's world z), the Text saying PC SETTINGS, the Mesh among the background's
//   children and both in the view's draw lists, and the panel, its glow and the background a row taller (their
//   vertices below the middle, 56 bytes each, moved down). The container's header is written 32 bytes long (the
//   original pads it to 216): the game goes by its length field, and the recompressed scene fits.
#include "amp_ark_overlay.h"
#include "amp_menu.h"

#include "bozo/libs/cd.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amp::menu
{
    namespace
    {
        using Bytes = std::vector<uint8_t>;

        uint32_t u32(const Bytes &b, size_t at)
        {
            uint32_t v;
            std::memcpy(&v, b.data() + at, 4);
            return v;
        }
        void putU32(Bytes &b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
        float f32(const Bytes &b, size_t at)
        {
            float v;
            std::memcpy(&v, b.data() + at, 4);
            return v;
        }
        void putF32(Bytes &b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
        void appendU32(Bytes &b, uint32_t v)
        {
            const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
            b.insert(b.end(), p, p + 4);
        }
        void appendString(Bytes &b, const std::string &s)
        {
            appendU32(b, static_cast<uint32_t>(s.size()));
            b.insert(b.end(), s.begin(), s.end());
        }
        size_t find(const Bytes &b, const Bytes &what, size_t from = 0)
        {
            if (what.empty() || b.size() < what.size())
                return std::string::npos;
            const auto it = std::search(b.begin() + static_cast<std::ptrdiff_t>(std::min(from, b.size())), b.end(),
                                        what.begin(), what.end());
            return it == b.end() ? std::string::npos : static_cast<size_t>(it - b.begin());
        }
        Bytes bytesOf(const std::string &s) { return Bytes(s.begin(), s.end()); }

        // ---- DataArray (binary) ------------------------------------------------------------------------------------

        struct List;
        struct Node
        {
            uint8_t kind = 1; // 0 int, 1 symbol, 2 float, 3 list
            uint32_t raw = 0; // int / float bits
            std::string sym;
            std::shared_ptr<List> list;
        };
        struct List
        {
            uint32_t id = 0, file = 0;
            std::vector<Node> items;
            bool head(const char *a, const char *b = nullptr) const
            {
                return items.size() >= (b ? 2u : 1u) && items[0].kind == 1 && items[0].sym == a &&
                       (!b || (items[1].kind == 1 && items[1].sym == b));
            }
        };
        struct DataFile
        {
            uint8_t version = 0;
            std::vector<std::string> files;
            std::shared_ptr<List> root;
        };

        Node symbol(const std::string &s)
        {
            Node n;
            n.kind = 1;
            n.sym = s;
            return n;
        }

        struct Reader
        {
            const Bytes &b;
            size_t at = 0;
            bool ok = true;
            bool need(size_t n)
            {
                if (at + n > b.size())
                    ok = false;
                return ok;
            }
            uint32_t word()
            {
                if (!need(4))
                    return 0;
                const uint32_t v = u32(b, at);
                at += 4;
                return v;
            }
            std::string string()
            {
                const uint32_t n = word();
                if (!need(n))
                    return {};
                std::string s(reinterpret_cast<const char *>(b.data() + at), n);
                at += n;
                return s;
            }
            std::shared_ptr<List> list(int depth = 0)
            {
                auto l = std::make_shared<List>();
                if (depth > 64 || !need(10))
                    return ok = false, l;
                const uint16_t count = static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
                at += 2;
                l->id = word();
                l->file = word();
                std::vector<uint32_t> kinds((count * 2u + 31u) / 32u);
                for (uint32_t &k : kinds)
                    k = word();
                for (uint32_t i = 0; i < count && ok; ++i)
                {
                    Node n;
                    n.kind = static_cast<uint8_t>((kinds[i / 16] >> (2 * (i % 16))) & 3u);
                    if (n.kind == 1)
                        n.sym = string();
                    else if (n.kind == 3)
                        n.list = list(depth + 1);
                    else
                        n.raw = word();
                    l->items.push_back(std::move(n));
                }
                return l;
            }
        };

        std::optional<DataFile> readData(const Bytes &b)
        {
            Reader r{b};
            DataFile f;
            if (b.empty())
                return std::nullopt;
            f.version = b[0];
            r.at = 1;
            const uint32_t count = r.word();
            for (uint32_t i = 0; i < count && r.ok && i < 4096; ++i)
                f.files.push_back(r.string());
            f.root = r.list();
            if (!r.ok || r.at != b.size())
                return std::nullopt;
            return f;
        }

        void writeList(const List &l, Bytes &out)
        {
            const uint16_t count = static_cast<uint16_t>(l.items.size());
            out.push_back(static_cast<uint8_t>(count));
            out.push_back(static_cast<uint8_t>(count >> 8));
            appendU32(out, l.id);
            appendU32(out, l.file);
            for (size_t w = 0; w < (l.items.size() * 2 + 31) / 32; ++w)
            {
                uint32_t word = 0;
                for (size_t i = 0; i < 16 && w * 16 + i < l.items.size(); ++i)
                    word |= static_cast<uint32_t>(l.items[w * 16 + i].kind) << (2 * i);
                appendU32(out, word);
            }
            for (const Node &n : l.items)
                if (n.kind == 1)
                    appendString(out, n.sym);
                else if (n.kind == 3)
                    writeList(*n.list, out);
                else
                    appendU32(out, n.raw);
        }

        Bytes writeData(const DataFile &f)
        {
            Bytes out{f.version};
            appendU32(out, static_cast<uint32_t>(f.files.size()));
            for (const std::string &s : f.files)
                appendString(out, s);
            writeList(*f.root, out);
            return out;
        }

        // The demo disc's own lists, taken out (the retail game never shows them).
        size_t dropLists(List &root, const std::vector<std::pair<const char *, const char *>> &heads)
        {
            const size_t before = root.items.size();
            std::erase_if(root.items, [&](const Node &n) {
                if (n.kind != 3)
                    return false;
                for (const auto &[a, b] : heads)
                    if (n.list->head(a, b))
                        return true;
                return false;
            });
            return before - root.items.size();
        }

        // The panel's buttons a PC SETTINGS more, `object` its scene's object (o_main_04, s_pause_06_but).
        bool addButton(List &root, const char *panel, const char *object)
        {
            for (Node &n : root.items)
            {
                if (n.kind != 3 || !n.list->head("freq_panel", panel))
                    continue;
                std::vector<Node> &items = n.list->items;
                size_t last = 0;
                for (size_t i = 0; i < items.size(); ++i)
                    if (items[i].kind == 3 && items[i].list->head("button_comp"))
                        last = i;
                if (!last)
                    return false;
                Node button;
                button.kind = 3;
                button.list = std::make_shared<List>();
                button.list->id = items[last].list->id;
                button.list->file = items[last].list->file;
                button.list->items = {symbol("button_comp"), symbol("pc_settings"), symbol("buttonStyle2"), symbol(object)};
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(last) + 1, button);
                for (Node &m : items)
                    if (m.kind == 3 && m.list->head("navigator"))
                        for (Node &v : m.list->items)
                            if (v.kind == 3 && v.list->head("vertical"))
                            {
                                v.list->items.push_back(symbol("pc_settings"));
                                return true;
                            }
                return false;
            }
            return false;
        }

        std::optional<Bytes> editMetaUi(const Bytes &original)
        {
            std::optional<DataFile> f = readData(original);
            if (!f || f->files.empty() || f->files[0] != "metagame/meta.ui")
                return std::nullopt;
            const size_t dropped = dropLists(*f->root, {{"mc_dialog_panel", "demo_freq_limit_error_dlg"},
                                                        {"dialog_panel", "net_load_config_error_dlg_demo"},
                                                        {"trans_error_screen", "net_load_config_error_demo"},
                                                        {"error_screen", "demo_freq_limit_error"}});
            if (dropped != 4 || !addButton(*f->root, "o_main", "04") || !addButton(*f->root, "s_pause", "06_but"))
                return std::nullopt;
            return writeData(*f);
        }

        std::optional<Bytes> editMessages(const Bytes &original)
        {
            std::optional<DataFile> f = readData(original);
            if (!f)
                return std::nullopt;
            const size_t dropped = dropLists(*f->root, {{"config_not_found_msg_demo", nullptr}, {"demo_freq_limit_error_dlg", nullptr}});
            const Node *like = nullptr;
            for (const Node &n : f->root->items)
                if (n.kind == 3 && n.list->head("o_main_credits_HELP"))
                    like = &n;
            if (dropped != 2 || !like)
                return std::nullopt;
            Node help;
            help.kind = 3;
            help.list = std::make_shared<List>();
            help.list->id = like->list->id;
            help.list->file = like->list->file;
            help.list->items = {symbol("o_main_pc_settings_HELP"), symbol("Video, cheats, save states and quit")};
            f->root->items.push_back(help);
            return writeData(*f);
        }

        // ---- The panels' scenes ------------------------------------------------------------------------------------

        constexpr uint32_t kContainer = 0xCCBEDEAFu, kHeader = 32u;
        constexpr float kRow = 24.0f;

        std::optional<Bytes> inflate(const Bytes &file)
        {
            if (file.size() < 20 || u32(file, 0) != kContainer || u32(file, 8) != 1u)
                return std::nullopt;
            const uint32_t header = u32(file, 4), size = u32(file, 12);
            if (header >= file.size() || size > (1u << 24))
                return std::nullopt;
            Bytes out(size);
            z_stream z{};
            if (inflateInit2(&z, 31) != Z_OK)
                return std::nullopt;
            z.next_in = const_cast<Bytef *>(file.data() + header);
            z.avail_in = static_cast<uInt>(file.size() - header);
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(out.size());
            const int r = ::inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (r != Z_STREAM_END || z.total_out != size)
                return std::nullopt;
            return out;
        }

        Bytes deflate(const Bytes &raw)
        {
            Bytes gz(compressBound(static_cast<uLong>(raw.size())) + 64);
            z_stream z{};
            deflateInit2(&z, 9, Z_DEFLATED, 31, 9, Z_DEFAULT_STRATEGY);
            z.next_in = const_cast<Bytef *>(raw.data());
            z.avail_in = static_cast<uInt>(raw.size());
            z.next_out = gz.data();
            z.avail_out = static_cast<uInt>(gz.size());
            ::deflate(&z, Z_FINISH);
            gz.resize(z.total_out);
            deflateEnd(&z);
            Bytes file(kHeader, 0xCC);
            putU32(file, 0, kContainer);
            putU32(file, 4, kHeader);
            putU32(file, 8, 1);
            putU32(file, 12, static_cast<uint32_t>(raw.size()));
            putU32(file, 16, static_cast<uint32_t>(gz.size()));
            file.insert(file.end(), gz.begin(), gz.end());
            return file;
        }

        struct Object
        {
            std::string type, name;
            Bytes data;
        };
        struct Scene
        {
            uint32_t version = 0;
            std::vector<Object> objects;
            Bytes tail;
            Object *find(const std::string &name)
            {
                for (Object &o : objects)
                    if (o.name == name)
                        return &o;
                return nullptr;
            }
            size_t index(const std::string &name) const
            {
                for (size_t i = 0; i < objects.size(); ++i)
                    if (objects[i].name == name)
                        return i;
                return std::string::npos;
            }
        };

        std::optional<Scene> parseScene(const Bytes &raw)
        {
            Reader r{raw};
            Scene s;
            s.version = r.word();
            const uint32_t count = r.word();
            if (!r.ok || count > 4096)
                return std::nullopt;
            for (uint32_t i = 0; i < count && r.ok; ++i)
            {
                Object o;
                o.type = r.string();
                o.name = r.string();
                s.objects.push_back(std::move(o));
            }
            const Bytes marker{0xAD, 0xDE, 0xAD, 0xDE};
            size_t at = r.at;
            for (Object &o : s.objects)
            {
                const size_t end = find(raw, marker, at);
                if (!r.ok || end == std::string::npos)
                    return std::nullopt;
                o.data.assign(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(end + 4));
                at = end + 4;
            }
            s.tail.assign(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.end());
            return s;
        }

        Bytes buildScene(const Scene &s)
        {
            Bytes out;
            appendU32(out, s.version);
            appendU32(out, static_cast<uint32_t>(s.objects.size()));
            for (const Object &o : s.objects)
            {
                appendString(out, o.type);
                appendString(out, o.name);
            }
            for (const Object &o : s.objects)
                out.insert(out.end(), o.data.begin(), o.data.end());
            out.insert(out.end(), s.tail.begin(), s.tail.end());
            return out;
        }

        void replaceAll(Bytes &b, const std::string &from, const std::string &to)
        {
            const Bytes f = bytesOf(from), t = bytesOf(to);
            for (size_t at = find(b, f); at != std::string::npos; at = find(b, f, at + t.size()))
            {
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + f.size()));
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), t.begin(), t.end());
            }
        }

        bool isName(const Bytes &b, size_t at, uint32_t n)
        {
            if (n < 3 || n > 40 || at + n > b.size())
                return false;
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint8_t c = b[at + k];
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
                    return false;
            }
            return true;
        }

        // The name list holding `after` (a u32 count, then the names as u32 length and bytes): `name` added at its end.
        bool addToList(Bytes &b, const std::string &after, const std::string &name)
        {
            Bytes key;
            appendString(key, after);
            const size_t at = find(b, key);
            if (at == std::string::npos)
                return false;
            // The list's first name: walk back over the names before `after` until the count before one matches.
            for (size_t start = at;;)
            {
                size_t end = start, names = 0;
                while (end + 4 <= b.size() && isName(b, end + 4, u32(b, end)))
                    end += 4 + u32(b, end), ++names;
                if (start >= 4 && u32(b, start - 4) == names)
                {
                    Bytes entry;
                    appendString(entry, name);
                    b.insert(b.begin() + static_cast<std::ptrdiff_t>(end), entry.begin(), entry.end());
                    putU32(b, start - 4, static_cast<uint32_t>(names + 1));
                    return true;
                }
                bool back = false;
                for (size_t step = 8; step <= 48 && step <= start; ++step)
                    if (u32(b, start - step) == step - 4 && isName(b, start - step + 4, static_cast<uint32_t>(step - 4)))
                    {
                        start -= step;
                        back = true;
                        break;
                    }
                if (!back)
                    return false;
            }
        }

        // A mesh's vertices (56 bytes each: the position first): the count after its own name's last mention.
        bool vertices(const Bytes &m, const std::string &name, size_t &first, uint32_t &count)
        {
            const Bytes key = bytesOf(name);
            size_t last = std::string::npos;
            for (size_t at = find(m, key); at != std::string::npos; at = find(m, key, at + 1))
                last = at + key.size();
            if (last == std::string::npos)
                return false;
            for (size_t at = last; at < last + 40 && at + 4 <= m.size(); ++at)
            {
                const uint32_t n = u32(m, at);
                if (n >= 4 && n <= 300 && at + 4 + n * 56u < m.size())
                {
                    first = at + 4;
                    count = n;
                    return true;
                }
            }
            return false;
        }

        // The box a row taller: its vertices below the middle moved down.
        bool grow(Object &o)
        {
            size_t first = 0;
            uint32_t count = 0;
            if (!vertices(o.data, o.name, first, count))
                return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t y = first + i * 56u + 4u;
                if (f32(o.data, y) < 0.0f)
                    putF32(o.data, y, f32(o.data, y) - kRow);
            }
            return true;
        }

        std::optional<Bytes> editScene(const Bytes &original, const std::string &prefix, const std::string &last,
                                       const std::string &next, const std::vector<std::string> &boxes)
        {
            std::optional<Bytes> raw = inflate(original);
            std::optional<Scene> s = raw ? parseScene(*raw) : std::nullopt;
            if (!s || buildScene(*s) != *raw)
                return std::nullopt;
            const std::string lt = prefix + last + ".txt", lm = prefix + last + ".mesh";
            const std::string nt = prefix + next + ".txt", nm = prefix + next + ".mesh";
            const std::string bg = prefix + "bg.mesh";
            Object *text = s->find(lt), *mesh = s->find(lm);
            if (!text || !mesh || text->data.size() < 0xa0 || mesh->data.size() < 0x70 || !s->find(bg))
                return std::nullopt;
            Object t{"Text", nt, text->data}, m{"Mesh", nm, mesh->data};
            replaceAll(m.data, lt, nt);
            replaceAll(m.data, lm, nm);
            // The mesh a row lower: its local y and its world z; the text's world z with it.
            const float z = f32(m.data, 0x64);
            putF32(m.data, 0x30, f32(m.data, 0x30) - kRow);
            putF32(m.data, 0x64, z - kRow);
            bool moved = false;
            for (size_t at = 0x40; at < 0x90 && !moved; ++at)
                if (std::fabs(f32(t.data, at) - z) < 1e-3f)
                {
                    putF32(t.data, at, z - kRow);
                    moved = true;
                }
            // The label, after the font's name (a u32, then the text as a u32 length and its bytes).
            const size_t font = find(t.data, bytesOf(".font"));
            if (!moved || font == std::string::npos || font + 13 > t.data.size())
                return std::nullopt;
            const size_t label = font + 5 + 4;
            const uint32_t length = u32(t.data, label);
            if (label + 4 + length > t.data.size())
                return std::nullopt;
            Bytes newLabel;
            appendString(newLabel, "PC SETTINGS");
            t.data.erase(t.data.begin() + static_cast<std::ptrdiff_t>(label), t.data.begin() + static_cast<std::ptrdiff_t>(label + 4 + length));
            t.data.insert(t.data.begin() + static_cast<std::ptrdiff_t>(label), newLabel.begin(), newLabel.end());
            s->objects.insert(s->objects.begin() + static_cast<std::ptrdiff_t>(s->index(lt) + 1), t);
            s->objects.insert(s->objects.begin() + static_cast<std::ptrdiff_t>(s->index(lm) + 1), m);
            // The background holds the buttons' meshes; the view draws the meshes and the texts.
            Object *view = nullptr;
            for (Object &o : s->objects)
                if (o.type == "View")
                    view = &o;
            if (!view || !addToList(s->find(bg)->data, lm, nm) || !addToList(view->data, lm, nm) || !addToList(view->data, lt, nt))
                return std::nullopt;
            for (const std::string &box : boxes)
                if (Object *o = s->find(box); !o || !grow(*o))
                    return std::nullopt;
            return deflate(buildScene(*s));
        }

        // ---- The disc ----------------------------------------------------------------------------------------------

        std::optional<Bytes> readArchive(bozo::libs::DiscImage &disc, uint32_t arkLbn, uint32_t offset, uint32_t size)
        {
            const uint32_t first = offset / 2048u, skip = offset % 2048u, sectors = (skip + size + 2047u) / 2048u;
            Bytes all(static_cast<size_t>(sectors) * 2048u);
            if (!disc.read(arkLbn + first, sectors, all.data()))
                return std::nullopt;
            return Bytes(all.begin() + skip, all.begin() + skip + size);
        }
    } // namespace

    bool prepare(const std::string &iso)
    {
        bozo::libs::DiscImage disc;
        std::string error;
        uint32_t arkLbn = 0, arkBytes = 0;
        if (!disc.open(iso, error) || !(disc.findFile("\\GEN\\MAIN.ARK;1", arkLbn, arkBytes) || disc.findFile("GEN/MAIN.ARK", arkLbn, arkBytes)))
        {
            std::fprintf(stderr, "[amp:menu] PC SETTINGS left out: the disc's GEN/MAIN.ARK not read (%s)\n", error.c_str());
            return false;
        }
        struct Edit
        {
            const char *name;
            std::optional<Bytes> (*make)(const Bytes &);
        };
        static const Edit edits[] = {
            {"meta.ui.bin", editMetaUi},
            {"english_messages.txt.bin", editMessages},
            {"o_main.rnd", [](const Bytes &b) { return editScene(b, "o_main_", "03", "04", {"o_main_panel.mesh", "o_main_panel_trans.mesh", "o_main_bg.mesh"}); }},
            {"s_pause.rnd", [](const Bytes &b) { return editScene(b, "s_pause_", "05_but", "06_but", {"s_pause_panel.mesh", "s_pause_panel_trans.mesh", "s_pause_bg.mesh"}); }},
        };
        std::vector<std::pair<const ark::Known *, Bytes>> made;
        for (const Edit &e : edits)
        {
            const ark::Known *k = nullptr;
            for (const ark::Known &x : ark::known())
                if (std::strcmp(x.name, e.name) == 0)
                    k = &x;
            std::optional<Bytes> original = k ? readArchive(disc, arkLbn, k->offset, k->size) : std::nullopt;
            std::optional<Bytes> edited = original ? e.make(*original) : std::nullopt;
            if (!edited || edited->size() > k->size)
            {
                std::fprintf(stderr, "[amp:menu] PC SETTINGS left out: %s %s\n", e.name,
                             !original ? "not read" : !edited ? "not as expected" : "does not fit");
                return false;
            }
            made.emplace_back(k, std::move(*edited));
        }
        for (auto &[k, bytes] : made)
            ark::replace(k->name, k->offset, k->size, std::move(bytes));
        std::printf("[amp:menu] PC SETTINGS in the Options and pause menus\n");
        return true;
    }
} // namespace amp::menu
