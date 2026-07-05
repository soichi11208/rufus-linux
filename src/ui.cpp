/*
 * ui.cpp: stilus-based main window (replaces the old GTK4 ui.c).
 *
 * Layout mirrors the original IDD_DIALOG structure:
 *   [Drive Properties]
 *     Device            : combo + refresh
 *     Boot selection    : combo + SELECT
 *     Image option      : combo
 *     Partition scheme  : combo     Target system : combo
 *     [ ] List USB HDDs
 *     [ ] Old BIOS fixes
 *     [ ] UEFI runtime validation
 *   [Format Options]
 *     Volume label      : text input
 *     File system       : combo     Cluster size  : combo
 *     [ ] Quick format
 *     [ ] Extended label
 *     [ ] Check bad blocks            (passes combo)
 *     [ ] Persistent partition (LiveBoot)   size (MiB): text input
 *   Status              : label + progress bar
 *   [ START | CLOSE ]
 *
 * stilus has no native file-open dialog or modal-window primitive, so both
 * are built from scratch here: AppRoot is a small app-local widget that
 * layers an optional "modal" child (with a dimming scrim) over the
 * persistent form, and the two modals (file picker, start confirmation)
 * are just widget trees built into that slot.
 */
#define RUFUS_USE_STILUS 1
#include "rufus.h"
#include "worker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "stilus/gui.hpp"

using namespace stilus;

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Label has no text-wrapping support — split greedily on word boundaries so
// long messages (e.g. the destructive-write warning) still fit in a dialog.
std::vector<std::string> wrap_text_naive(const std::string &text, size_t max_chars) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        if (!cur.empty() && cur.size() + 1 + word.size() > max_chars) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!cur.empty()) cur += ' ';
        cur += word;
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::string join_path(const std::string &dir, const std::string &name) {
    if (!dir.empty() && dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string dirname_of(const std::string &path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

struct DirEntry {
    std::string name;
    bool        is_dir;
};

// Lists `path`, filtering regular files to *.iso/*.img (case-insensitive).
// ".." is included (unless at the filesystem root) so the picker can climb
// back up. Directories sort first, then alphabetically.
std::vector<DirEntry> list_dir(const std::string &path) {
    std::vector<DirEntry> out;
    DIR *d = opendir(path.c_str());
    if (!d) return out;

    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == ".") continue;
        if (name == ".." && path == "/") continue;

        std::string full = join_path(path, name);
        struct stat st{};
        bool is_dir = false;
        if (stat(full.c_str(), &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
        } else {
            continue;   // broken symlink / permission denied — skip
        }

        if (!is_dir && name != "..") {
            std::string lower = name;
            for (auto &ch : lower) ch = char(tolower((unsigned char)ch));
            bool iso = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".iso") == 0;
            bool img = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".img") == 0;
            if (!iso && !img) continue;
        }
        out.push_back({name, is_dir});
    }
    closedir(d);

    std::sort(out.begin(), out.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
    return out;
}

std::string format_size_gib(uint64_t bytes) {
    double gib = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f GiB", gib);
    return buf;
}

// ---------------------------------------------------------------------------
// AppRoot — persistent form + an optional centered modal over a dim scrim.
// ---------------------------------------------------------------------------
class AppRoot : public Widget {
public:
    explicit AppRoot(std::unique_ptr<Widget> form) : form_(std::move(form)) {
        form_->set_parent(this);
    }

    void show_modal(std::unique_ptr<Widget> content) {
        modal_ = std::move(content);
        if (modal_) modal_->set_parent(this);
        invalidate();
    }
    void close_modal() {
        if (modal_) { modal_.reset(); invalidate(); }
    }
    bool has_modal() const { return bool(modal_); }

    Size measure(const Constraints &c) override {
        form_->measure(c);
        if (modal_) modal_->measure(c.loosen());
        return {c.max_w, c.max_h};
    }

    void layout(Rect r) override {
        rect_ = r;
        form_->layout(r);
        if (modal_) {
            Constraints cc{0, r.w * 0.92f, 0, r.h * 0.88f};
            Size sz = modal_->measure(cc);
            float w = std::min(sz.w, cc.max_w);
            float h = std::min(sz.h, cc.max_h);
            Rect mr{r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h};
            modal_->layout(mr);
        }
    }

    void paint(Canvas &c, const Theme &t) override { form_->paint(c, t); }

    void paint_overlay(Canvas &c, const Theme &t) override {
        form_->paint_overlay(c, t);
        if (modal_) {
            c.fill_rect(rect_, Color{0.f, 0.f, 0.f, 0.55f});
            modal_->paint(c, t);
            modal_->paint_overlay(c, t);
        }
    }

    bool dispatch_event(const Event &e) override {
        if (modal_) {
            // Swallow everything while a modal is up — the dimmed form
            // beneath never reacts. Both modals always offer an explicit
            // Cancel, so there is no need for click-outside-to-dismiss.
            modal_->dispatch_event(e);
            return true;
        }
        return form_->dispatch_event(e);
    }

    void collect_focusable(std::vector<Widget *> &out) override {
        if (modal_) modal_->collect_focusable(out);
        else form_->collect_focusable(out);
    }

    Widget *hit(Vec2 p) override { return modal_ ? modal_->hit(p) : form_->hit(p); }

    size_t  child_count() const override { return modal_ ? 2 : 1; }
    Widget *child(size_t i) override { return i == 0 ? form_.get() : modal_.get(); }

private:
    std::unique_ptr<Widget> form_;
    std::unique_ptr<Widget> modal_;
};

// ---------------------------------------------------------------------------
// AppUi — every widget pointer the app logic needs to read/mutate, plus the
// Window/AppRoot it lives in. Analogous to the old `rufus_ui_t`.
// ---------------------------------------------------------------------------
struct AppUi {
    Window  *win  = nullptr;
    AppRoot *root = nullptr;

    ComboBox   *device      = nullptr;
    ComboBox   *boot_sel    = nullptr;
    Button     *select_btn  = nullptr;
    ComboBox   *image_opt   = nullptr;
    ComboBox   *part_scheme = nullptr;
    ComboBox   *target_sys  = nullptr;

    CheckBox *list_usb_hdd    = nullptr;
    CheckBox *old_bios_fixes  = nullptr;
    CheckBox *uefi_validation = nullptr;

    TextInput *label   = nullptr;
    ComboBox  *fs       = nullptr;
    ComboBox  *cluster  = nullptr;

    CheckBox *quick_fmt  = nullptr;
    CheckBox *ext_label  = nullptr;
    CheckBox *bad_blocks = nullptr;
    ComboBox *nb_passes  = nullptr;

    CheckBox  *persistent      = nullptr;
    TextInput *persistent_size = nullptr;   // MiB, parsed on demand

    Label       *status   = nullptr;
    ProgressBar *progress = nullptr;
    Button      *start_btn = nullptr;
    Button      *close_btn = nullptr;
};

AppUi ui;

// UI dropdown index -> boot_type_t mapping (order must match boot_items).
enum { UI_BOOT_IMAGE = 0, UI_BOOT_FREEDOS = 1, UI_BOOT_NONBOOT = 2 };

void set_status(const std::string &s) {
    if (ui.status) ui.status->text(s);
}

// ---------------------------------------------------------------------------
// Device (re)scan
// ---------------------------------------------------------------------------
void refresh_drives() {
    drive_free(&g_state);
    int n = drive_scan(&g_state);
    if (n < 0) {
        set_status(_("Failed to scan drives"));
        return;
    }

    std::vector<ComboBox::Item> items;
    if (n == 0) {
        items.push_back({_("(no USB drive detected)"), 0});
    } else {
        for (size_t i = 0; i < g_state.drive_count; i++) {
            const drive_info_t &d = g_state.drives[i];
            char line[256];
            snprintf(line, sizeof line, "%.32s  [%s]  %.96s",
                     d.devnode, format_size_gib(d.size_bytes).c_str(),
                     d.model[0] ? d.model : "(unknown)");
            items.push_back({line, int(i)});
        }
    }
    ui.device->set_items(std::move(items));
    if (n > 0) ui.device->select(0);
    g_state.selected_drive = (n > 0) ? 0 : -1;

    char msg[128];
    snprintf(msg, sizeof msg, _("%d device(s) found"), n);
    set_status(msg);
    rufus_log("Drive scan: %d device(s) found", n);
}

// ---------------------------------------------------------------------------
// File picker modal
// ---------------------------------------------------------------------------
void open_file_picker(const std::string &start_dir);

std::unique_ptr<Widget> build_file_picker_content(const std::string &dir) {
    auto col = column();
    col->padding(14).gap(8).cross(CrossAlign::Stretch);

    auto title = std::make_unique<Label>(_("Select image"));
    title->bold().font_size(18);
    col->add(std::move(title), 0);

    auto path_lbl = std::make_unique<Label>(dir);
    path_lbl->color(Color::rgb(0x9aa0a6));
    col->add(std::move(path_lbl), 0);
    col->add(std::make_unique<Divider>(), 0);

    auto scroll = std::make_unique<ScrollView>();
    auto list = column();
    list->gap(2).cross(CrossAlign::Stretch);

    for (const auto &entry : list_dir(dir)) {
        std::string label = entry.name;
        if (entry.name == "..") label = "../";
        else if (entry.is_dir)  label = entry.name + "/";

        auto btn = std::make_unique<Button>(label);
        std::string full = (entry.name == "..") ? dirname_of(dir) : join_path(dir, entry.name);
        bool is_dir = entry.is_dir || entry.name == "..";
        btn->on_click([full, is_dir]() {
            if (is_dir) {
                open_file_picker(full);
            } else {
                strncpy(g_state.image_path, full.c_str(), sizeof g_state.image_path - 1);
                boot_type_t t = BOOT_NONE;
                iso_inspect(full.c_str(), &t);
                g_state.boot_type = t;

                size_t slash = full.find_last_of('/');
                std::string base = slash == std::string::npos ? full : full.substr(slash + 1);
                if (ui.label) ui.label->set_text(base);

                set_status(std::string(_("Image: ")) + full);
                rufus_log("Selected image: %s", full.c_str());
                ui.root->close_modal();
            }
        });
        list->add(std::move(btn), 0);
    }
    scroll->add(std::move(list));
    col->add(std::move(scroll), 1);

    auto btn_row = row();
    btn_row->gap(8);
    auto cancel = std::make_unique<Button>(_("Cancel"));
    cancel->on_click([]() { ui.root->close_modal(); });
    btn_row->add(std::move(cancel), 0);
    col->add(std::move(btn_row), 0);

    auto panel = std::make_unique<Panel>();
    panel->padding(4).child(std::move(col));
    return panel;
}

void open_file_picker(const std::string &start_dir) {
    ui.root->show_modal(build_file_picker_content(start_dir));
}

void on_select_clicked() {
    std::string start = g_state.image_path[0]
        ? dirname_of(g_state.image_path)
        : (getenv("HOME") ? getenv("HOME") : "/");
    open_file_picker(start);
}

// ---------------------------------------------------------------------------
// Confirmation modal + format kickoff
// ---------------------------------------------------------------------------
void begin_format();

std::unique_ptr<Widget> build_confirm_dialog(const std::string &message) {
    auto col = column();
    col->padding(18).gap(12).cross(CrossAlign::Stretch);

    for (const auto &line : wrap_text_naive(message, 48)) {
        col->add(std::make_unique<Label>(line), 0);
    }

    auto btn_row = row();
    btn_row->gap(8);
    auto spacer_w = std::make_unique<Spacer>();
    btn_row->add(std::move(spacer_w), 1);

    auto cancel = std::make_unique<Button>(_("Cancel"));
    cancel->on_click([]() { ui.root->close_modal(); });
    btn_row->add(std::move(cancel), 0);

    auto start = std::make_unique<Button>(_("Start"));
    start->primary();
    start->on_click([]() {
        ui.root->close_modal();
        begin_format();
    });
    btn_row->add(std::move(start), 0);
    col->add(std::move(btn_row), 0);

    auto panel = std::make_unique<Panel>();
    panel->title(_("Confirm")).padding(4).child(std::move(col));
    return panel;
}

// ---------------------------------------------------------------------------
// Worker progress / completion (invoked from worker_poll(), on the UI thread)
// ---------------------------------------------------------------------------
void on_worker_progress(double fraction, const char *status, void *) {
    if (fraction >= 0.0 && fraction <= 1.0 && ui.progress) ui.progress->value(float(fraction));
    if (status && *status) set_status(status);
}

void on_worker_done(int rc, void *) {
    if (ui.start_btn) ui.start_btn->set_focused(false);
    // Buttons have no explicit enable/disable flag in stilus yet — reflect
    // "job finished" purely through status text; the button remains
    // clickable but begin_format() re-validates state on each press anyway.
    if (rc == 0) {
        if (ui.progress) ui.progress->value(1.0f);
        set_status(_("DONE"));
        rufus_log("Write complete.");
    } else {
        set_status(_("Failed — see log"));
        rufus_log("Write failed (rc=%d).", rc);
    }
}

void begin_format() {
    if (!privops_have_root()) {
        set_status(_("Root required — re-run with `sudo` or `pkexec`."));
        rufus_log("Not running as root; aborting write.");
        return;
    }

    format_job_t job{};
    job.drive = g_state.drives[g_state.selected_drive];

    strncpy(job.image_path, g_state.image_path, sizeof job.image_path - 1);
    job.boot_type = g_state.boot_type;

    job.fs_type           = (fs_type_t)ui.fs->selected_index();
    job.partition_scheme  = (partition_scheme_t)ui.part_scheme->selected_index();
    job.target_system     = (target_system_t)ui.target_sys->selected_index();

    const std::string &lbl = ui.label->text();
    strncpy(job.volume_label, lbl.c_str(), sizeof job.volume_label - 1);

    job.quick_format          = ui.quick_fmt->checked();
    job.check_bad_blocks      = ui.bad_blocks->checked();
    job.bad_block_passes      = ui.nb_passes->selected_index() + 1;   // 0-based -> 1-based
    job.old_bios_fixes        = ui.old_bios_fixes->checked();
    job.uefi_media_validation = ui.uefi_validation->checked();
    job.persistent            = ui.persistent->checked();

    uint32_t size_mb = uint32_t(strtoul(ui.persistent_size->text().c_str(), nullptr, 10));
    job.persistent_size_mb = size_mb;

    g_state.list_usb_hdds = ui.list_usb_hdd->checked();

    if (ui.progress) ui.progress->value(0.f);
    set_status(_("Starting…"));
    worker_run_format(&job, on_worker_progress, on_worker_done, nullptr);
}

void on_start_clicked() {
    if (g_state.selected_drive < 0 ||
        (size_t)g_state.selected_drive >= g_state.drive_count) {
        set_status(_("Select a device first."));
        return;
    }
    if (g_state.boot_type != BOOT_NON_BOOTABLE && !g_state.image_path[0]) {
        set_status(_("Select an image first."));
        return;
    }

    const drive_info_t &d = g_state.drives[g_state.selected_drive];
    if (d.size_bytes == 0) {
        set_status(_("Selected device reports 0 bytes — no media inserted, "
                     "or the drive is not readable. Re-plug it and Refresh."));
        rufus_log("Refusing to write: %s reports 0 bytes (no media).", d.devnode);
        return;
    }

    char msg[512];
    snprintf(msg, sizeof msg,
             _("ALL data on %.32s (%s, %.96s) will be destroyed. Continue?"),
             d.devnode, format_size_gib(d.size_bytes).c_str(),
             d.model[0] ? d.model : _("unknown"));

    ui.root->show_modal(build_confirm_dialog(msg));
}

// ---------------------------------------------------------------------------
// Settings sync (mirrors the old ui_sync_to_state, called on window close)
// ---------------------------------------------------------------------------
void ui_sync_to_state() {
    g_state.partition_scheme      = (partition_scheme_t)ui.part_scheme->selected_index();
    g_state.target_system         = (target_system_t)ui.target_sys->selected_index();
    g_state.fs_type                = (fs_type_t)ui.fs->selected_index();
    g_state.cluster_size           = (uint32_t)std::max(0, ui.cluster->selected_index());
    g_state.quick_format            = ui.quick_fmt->checked();
    g_state.check_bad_blocks        = ui.bad_blocks->checked();
    g_state.bad_block_passes        = ui.nb_passes->selected_index() + 1;
    g_state.list_usb_hdds           = ui.list_usb_hdd->checked();
    g_state.old_bios_fixes          = ui.old_bios_fixes->checked();
    g_state.uefi_media_validation   = ui.uefi_validation->checked();
    g_state.persistent              = ui.persistent->checked();
    g_state.persistent_size_mb      = (uint32_t)strtoul(ui.persistent_size->text().c_str(), nullptr, 10);
    strncpy(g_state.volume_label, ui.label->text().c_str(), sizeof g_state.volume_label - 1);
}

// ---------------------------------------------------------------------------
// Widget tree construction
// ---------------------------------------------------------------------------
std::unique_ptr<Widget> labeled_row(const char *label, std::unique_ptr<Widget> w) {
    auto r = row();
    r->gap(10).cross(CrossAlign::Center);
    r->add(std::make_unique<Label>(label), 0, 130);
    r->add(std::move(w), 1);
    return r;
}

std::unique_ptr<Widget> section_label(const char *text) {
    auto lbl = std::make_unique<Label>(text);
    lbl->bold().font_size(16);
    return lbl;
}

std::unique_ptr<Widget> build_form() {
    auto outer = column();
    outer->padding(16).gap(10).cross(CrossAlign::Stretch);

    // ---------- Drive Properties ----------
    outer->add(section_label(_("Drive Properties")), 0);
    outer->add(std::make_unique<Divider>(), 0);

    {
        auto dev_row = row();
        dev_row->gap(8).cross(CrossAlign::Center);
        auto device = std::make_unique<ComboBox>(
            std::vector<ComboBox::Item>{{_("(click refresh to scan)"), 0}});
        ui.device = device.get();
        dev_row->add(std::move(device), 1);
        auto refresh = std::make_unique<Button>(_("Refresh"));
        refresh->on_click([]() { refresh_drives(); });
        dev_row->add(std::move(refresh), 0);
        outer->add(labeled_row(_("Device"), std::move(dev_row)), 0);
    }

    {
        auto boot_row = row();
        boot_row->gap(8).cross(CrossAlign::Center);
        std::vector<ComboBox::Item> items = {
            {_("Disk or ISO image (Please select)"), UI_BOOT_IMAGE},
            {_("FreeDOS"),                            UI_BOOT_FREEDOS},
            {_("Non bootable"),                       UI_BOOT_NONBOOT},
        };
        auto boot_sel = std::make_unique<ComboBox>(std::move(items));
        boot_sel->select(UI_BOOT_IMAGE);
        boot_sel->on_select([](int idx, const std::string &) {
            switch (idx) {
            case UI_BOOT_IMAGE:   break;   // type is set by iso_inspect on file select
            case UI_BOOT_FREEDOS: g_state.boot_type = BOOT_FREEDOS;      break;
            case UI_BOOT_NONBOOT: g_state.boot_type = BOOT_NON_BOOTABLE; break;
            default: break;
            }
        });
        ui.boot_sel = boot_sel.get();
        boot_row->add(std::move(boot_sel), 1);

        auto select_btn = std::make_unique<Button>(_("SELECT"));
        select_btn->on_click(on_select_clicked);
        ui.select_btn = select_btn.get();
        boot_row->add(std::move(select_btn), 0);
        outer->add(labeled_row(_("Boot selection"), std::move(boot_row)), 0);
    }

    {
        std::vector<ComboBox::Item> items = {
            {_("Standard Windows installation"), 0},
            {_("Windows To Go"),                 1},
        };
        auto image_opt = std::make_unique<ComboBox>(std::move(items));
        image_opt->select(0);
        ui.image_opt = image_opt.get();
        outer->add(labeled_row(_("Image option"), std::move(image_opt)), 0);
    }

    {
        auto grid = row();
        grid->gap(12).cross(CrossAlign::Stretch);

        auto ps_col = column();
        ps_col->gap(2);
        ps_col->add(std::make_unique<Label>(_("Partition scheme")), 0);
        auto ps = std::make_unique<ComboBox>(
            std::vector<ComboBox::Item>{{"MBR", 0}, {"GPT", 1}});
        ps->select(int(g_state.partition_scheme));
        ui.part_scheme = ps.get();
        ps_col->add(std::move(ps), 0);
        grid->add(std::move(ps_col), 1);

        auto ts_col = column();
        ts_col->gap(2);
        ts_col->add(std::make_unique<Label>(_("Target system")), 0);
        auto ts = std::make_unique<ComboBox>(std::vector<ComboBox::Item>{
            {_("BIOS or UEFI"), 0}, {_("UEFI (non CSM)"), 1}});
        ts->select(int(g_state.target_system));
        ui.target_sys = ts.get();
        ts_col->add(std::move(ts), 0);
        grid->add(std::move(ts_col), 1);

        outer->add(std::move(grid), 0);
    }

    {
        auto list_usb_hdd = std::make_unique<CheckBox>(
            _("List USB Hard Drives"), g_state.list_usb_hdds);
        ui.list_usb_hdd = list_usb_hdd.get();
        outer->add(std::move(list_usb_hdd), 0);

        auto old_bios = std::make_unique<CheckBox>(
            _("Add fixes for old BIOSes (extra partition, align, etc.)"),
            g_state.old_bios_fixes);
        ui.old_bios_fixes = old_bios.get();
        outer->add(std::move(old_bios), 0);

        auto uefi_val = std::make_unique<CheckBox>(
            _("Enable runtime UEFI media validation"), g_state.uefi_media_validation);
        ui.uefi_validation = uefi_val.get();
        outer->add(std::move(uefi_val), 0);
    }

    // ---------- Format Options ----------
    outer->add(section_label(_("Format Options")), 0);
    outer->add(std::make_unique<Divider>(), 0);

    {
        auto label = std::make_unique<TextInput>(_("Volume label"));
        label->set_text(g_state.volume_label);
        ui.label = label.get();
        outer->add(labeled_row(_("Volume label"), std::move(label)), 0);
    }

    {
        auto grid = row();
        grid->gap(12).cross(CrossAlign::Stretch);

        auto fs_col = column();
        fs_col->gap(2);
        fs_col->add(std::make_unique<Label>(_("File system")), 0);
        std::vector<ComboBox::Item> fs_items = {
            {"FAT32", 0}, {"exFAT", 1}, {"NTFS", 2},
            {"ext4", 3}, {"btrfs", 4}, {"UDF", 5},
        };
        auto fs = std::make_unique<ComboBox>(std::move(fs_items));
        fs->select(int(g_state.fs_type));
        ui.fs = fs.get();
        fs_col->add(std::move(fs), 0);
        grid->add(std::move(fs_col), 1);

        auto cs_col = column();
        cs_col->gap(2);
        cs_col->add(std::make_unique<Label>(_("Cluster size")), 0);
        std::vector<ComboBox::Item> cs_items = {
            {_("Default"), 0}, {_("4096 bytes"), 1}, {_("8192 bytes"), 2},
            {_("16384 bytes"), 3}, {_("32768 bytes"), 4}, {_("65536 bytes"), 5},
        };
        auto cluster = std::make_unique<ComboBox>(std::move(cs_items));
        cluster->select(int(g_state.cluster_size));
        ui.cluster = cluster.get();
        cs_col->add(std::move(cluster), 0);
        grid->add(std::move(cs_col), 1);

        outer->add(std::move(grid), 0);
    }

    {
        auto quick_fmt = std::make_unique<CheckBox>(_("Quick format"), g_state.quick_format);
        ui.quick_fmt = quick_fmt.get();
        outer->add(std::move(quick_fmt), 0);

        auto ext_label = std::make_unique<CheckBox>(_("Create extended label and icon files"), true);
        ui.ext_label = ext_label.get();
        outer->add(std::move(ext_label), 0);
    }

    {
        auto bb_row = row();
        bb_row->gap(10).cross(CrossAlign::Center);
        auto bad_blocks = std::make_unique<CheckBox>(
            _("Check device for bad blocks"), g_state.check_bad_blocks);
        ui.bad_blocks = bad_blocks.get();
        bb_row->add(std::move(bad_blocks), 1);

        std::vector<ComboBox::Item> pass_items = {
            {_("1 pass"), 0}, {_("2 passes"), 1}, {_("3 passes"), 2}, {_("4 passes"), 3},
        };
        auto nb_passes = std::make_unique<ComboBox>(std::move(pass_items));
        int passes_idx = (g_state.bad_block_passes >= 1 && g_state.bad_block_passes <= 4)
                             ? g_state.bad_block_passes - 1 : 0;
        nb_passes->select(passes_idx);
        ui.nb_passes = nb_passes.get();
        bb_row->add(std::move(nb_passes), 0, 140);
        outer->add(std::move(bb_row), 0);
    }

    {
        auto persist_row = row();
        persist_row->gap(10).cross(CrossAlign::Center);
        auto persistent = std::make_unique<CheckBox>(
            _("Create persistent partition (LiveBoot)"), g_state.persistent);
        CheckBox *persistent_raw = persistent.get();
        persist_row->add(std::move(persistent), 1);

        auto size_input = std::make_unique<TextInput>(_("Size (MiB, 0=all free space)"));
        char sizebuf[16];
        snprintf(sizebuf, sizeof sizebuf, "%u",
                 g_state.persistent_size_mb > 0 ? g_state.persistent_size_mb : 4096);
        size_input->set_text(sizebuf);
        ui.persistent_size = size_input.get();
        persist_row->add(std::move(size_input), 0, 140);

        ui.persistent = persistent_raw;
        outer->add(std::move(persist_row), 0);
    }

    // ---------- Status + progress ----------
    outer->add(section_label(_("Status")), 0);
    outer->add(std::make_unique<Divider>(), 0);

    auto status = std::make_unique<Label>(_("READY"));
    ui.status = status.get();
    outer->add(std::move(status), 0);

    auto progress = std::make_unique<ProgressBar>();
    ui.progress = progress.get();
    outer->add(std::move(progress), 0);

    // ---------- Action buttons ----------
    {
        auto btn_row = row();
        btn_row->gap(8);
        btn_row->add(std::make_unique<Spacer>(), 1);

        auto start_btn = std::make_unique<Button>(_("START"));
        start_btn->primary();
        start_btn->on_click(on_start_clicked);
        ui.start_btn = start_btn.get();
        btn_row->add(std::move(start_btn), 0);

        auto close_btn = std::make_unique<Button>(_("CLOSE"));
        close_btn->on_click([]() {
            ui_sync_to_state();
            ui.win->close();
        });
        ui.close_btn = close_btn.get();
        btn_row->add(std::move(close_btn), 0);

        outer->add(std::move(btn_row), 0);
    }

    return outer;
}

} // namespace

// ---------------------------------------------------------------------------
int rufus_run_ui()
{
    Window win("Rufus " RUFUS_VERSION, 480, 700);
    if (!win.is_open()) return 1;

    // Primary + CJK fallback so Japanese labels (translated UI strings) and
    // IME preedit/commit text (volume label input) render as glyphs rather
    // than tofu boxes.
    auto load_with_cjk = [](const char* main, float px) {
        auto f = Font::from_file(main, px);
        if (f.valid()) {
            auto cjk = Font::from_file(
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", px, 0);
            if (cjk.valid()) f.add_fallback(std::move(cjk));
        }
        return f;
    };
    win.theme().font      = std::make_shared<Font>(
        load_with_cjk("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",      15.0f));
    win.theme().font_bold = std::make_shared<Font>(
        load_with_cjk("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15.0f));

    // Wrap the whole form in a ScrollView so the window remains usable
    // when the user's font/theme leaves the content taller than the
    // window (or when the window is deliberately resized short).
    // show_frame(false) suppresses the ScrollView's own rounded border,
    // which would look wrong wrapping the entire form.
    auto scrollable_form = std::make_unique<ScrollView>();
    scrollable_form->show_frame(false);
    scrollable_form->add(build_form());

    auto app_root = std::make_unique<AppRoot>(std::move(scrollable_form));
    ui.root = app_root.get();
    ui.win  = &win;

    win.set_root(std::move(app_root));

    // set_root() wires its own event_cb that dispatches into the widget
    // tree; calling on_event() afterward replaces that callback outright
    // (it does not chain), so we must re-dispatch to the tree ourselves —
    // matching the pattern examples/widget_demo.cpp uses for the same
    // reason.
    win.on_event([&](const Event &e) {
        if (e.type == Event::Type::Close) {
            ui_sync_to_state();
            win.close();
            return;
        }
        if (ui.root) ui.root->dispatch_event(e);
    });

    // Poll the worker's progress queue every frame; re-arm continuously for
    // the lifetime of the window (matches the anim_demo.cpp pattern).
    std::function<void(float)> tick;
    tick = [&](float) {
        worker_poll();
        win.request_animation_frame(tick);
    };
    win.request_animation_frame(tick);

    refresh_drives();

    return App::instance().run();
}
