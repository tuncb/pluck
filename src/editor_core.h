#pragma once
#include "capture.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pluck
{
namespace fs = std::filesystem;

enum class ToolKind
{
    Select,
    Pen,
    Highlighter,
    Arrow,
    Rectangle,
    Oval,
    Line,
    Text,
    Step,
    Redact,
    Crop
};

enum class AnnotationKind
{
    Pen,
    Highlighter,
    Arrow,
    Rectangle,
    Oval,
    Line,
    Text,
    Step,
    Redact
};

enum class SizePreset : int
{
    Small = 0,
    Medium = 1,
    Large = 2,
    ExtraLarge = 3
};

enum class HandleKind
{
    None,
    Body,
    Start,
    End,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left
};

struct PointF
{
    float x{};
    float y{};
};

struct RectF
{
    float left{};
    float top{};
    float right{};
    float bottom{};
};

constexpr std::array<COLORREF, 8> MarkupColors{
    RGB(239, 68, 68),   // Red
    RGB(249, 115, 22),  // Orange
    RGB(250, 204, 21),  // Yellow
    RGB(16, 185, 129),  // Emerald
    RGB(59, 130, 246),  // Blue
    RGB(168, 85, 247),  // Violet
    RGB(255, 255, 255), // White
    RGB(24, 24, 27),    // Charcoal
};

constexpr COLORREF DefaultInkColor = MarkupColors[0];
constexpr COLORREF DefaultHighlighterColor = MarkupColors[2];
constexpr COLORREF DefaultRedactColor = MarkupColors[7];

struct Annotation
{
    std::uint64_t id{};
    AnnotationKind kind{AnnotationKind::Pen};
    COLORREF color{DefaultInkColor};
    SizePreset size{SizePreset::Medium};
    // Rectangle/Oval: tinted fill; Text: solid badge pill; Redact: solid color box instead of pixelate.
    bool filled{};
    std::vector<PointF> points;
    PointF start{};
    PointF end{};
    std::wstring text;
    int step_number{1};
    std::uint64_t revision{1};
};

struct HighlighterPatch
{
    RECT bounds{};
    std::vector<std::uint32_t> premul_bgra;
};

struct PixelatePatch
{
    RECT bounds{};
    std::vector<std::uint32_t> bgra;
};

float stroke_width_for(AnnotationKind kind, SizePreset size) noexcept;
float font_size_for(SizePreset size) noexcept;
float step_radius_for(SizePreset size) noexcept;
int pixelate_block_for(SizePreset size) noexcept;
bool is_light_color(COLORREF color) noexcept;
D2D1_COLOR_F to_d2d_color(COLORREF color, float alpha = 1.0f) noexcept;

PointF constrain_angle_45(PointF start, PointF end) noexcept;
PointF constrain_square(PointF start, PointF end) noexcept;
RectF normalize_rect(PointF a, PointF b) noexcept;
RECT clamp_crop_rect(RECT rect, int max_width, int max_height) noexcept;

void measure_text_annotation(IDWriteFactory* dwrite, const Annotation& item, float& out_width,
                             float& out_height,
                             Microsoft::WRL::ComPtr<IDWriteTextLayout>* out_layout = nullptr);
RectF annotation_bounds(const Annotation& item, IDWriteFactory* dwrite = nullptr);
bool hit_test_annotation(const Annotation& item, PointF point, float tolerance,
                         IDWriteFactory* dwrite = nullptr);
HandleKind hit_test_handles(const Annotation& item, PointF point, float handle_radius,
                            IDWriteFactory* dwrite = nullptr);
void translate_annotation(Annotation& item, float dx, float dy) noexcept;
void resize_annotation(Annotation& item, HandleKind handle, PointF target, bool keep_aspect,
                       const RectF& initial_bounds, const std::vector<PointF>& initial_points) noexcept;

PixelatePatch compute_pixelate_patch(const Bitmap& base, const Annotation& item);
HighlighterPatch compute_highlighter_patch(const Bitmap& base, const Annotation& item, ID2D1Factory* d2d,
                                           IWICImagingFactory* wic);
void draw_vector_annotation(ID2D1RenderTarget* target, ID2D1Factory* d2d, IDWriteFactory* dwrite,
                            const Annotation& item, bool preview_highlighter = false);

class EditorDocument
{
  public:
    explicit EditorDocument(std::unique_ptr<Bitmap> base_image);
    const Bitmap& base() const noexcept;
    int base_width() const noexcept;
    int base_height() const noexcept;
    RECT crop_rect() const noexcept;
    bool is_cropped() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    const std::vector<Annotation>& annotations() const noexcept;
    const Annotation* find_annotation(std::uint64_t id) const noexcept;
    Annotation* find_annotation_mutable(std::uint64_t id) noexcept;
    std::uint64_t hit_test(PointF image_point, float tolerance, IDWriteFactory* dwrite = nullptr) const;
    int next_step_number() const noexcept;
    void reset_step_number() noexcept;

    std::uint64_t add_annotation(Annotation item);
    bool delete_annotation(std::uint64_t id);
    bool delete_annotations(const std::vector<std::uint64_t>& ids);
    bool clear_annotations();
    bool reset_all();
    bool set_crop_rect(RECT rect);
    bool reset_crop();
    bool update_annotation_style(std::uint64_t id, COLORREF color, SizePreset size, bool filled);
    bool update_annotation_text(std::uint64_t id, std::wstring text);

    void begin_mutation();
    void commit_mutation();
    void cancel_mutation();
    void mark_dirty() noexcept;

    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    bool undo();
    bool redo();
    std::uint64_t revision() const noexcept;

    const Bitmap& render_output() const;
    GlobalMemory make_clipboard_dib() const;
    void save_png(const fs::path& path) const;

  private:
    struct State
    {
        std::vector<Annotation> annotations;
        RECT crop{};
        int next_step{1};
    };

    std::unique_ptr<Bitmap> base_;
    State state_;
    std::uint64_t next_id_{1};
    std::uint64_t revision_{1};
    std::vector<State> undo_stack_;
    std::vector<State> redo_stack_;
    bool mutating_{};
    State mutation_snapshot_;
    mutable std::unique_ptr<Bitmap> cached_output_;
    mutable std::uint64_t cached_revision_{0};

    void push_undo_state(State previous);
};

void save_png(const Bitmap& source, const fs::path& path);
std::wstring default_save_filename(const SYSTEMTIME& local_time);
fs::path default_save_folder();
} // namespace pluck
