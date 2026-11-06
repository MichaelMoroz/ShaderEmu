"""Makes linux/imgui/imgui.patch's changes in a clean Dear ImGui 1.91.5 tree (docs/apps/imgui.md, "Drawn later"):
    python3 make_patch_edits.py IMGUI_TREE
then `git -C IMGUI_TREE diff > linux/imgui/imgui.patch`. Each edit must find its text once."""
import sys, os

tree = sys.argv[1]


def edit(name, old, new, count=1):
    path = os.path.join(tree, name)
    text = open(path, encoding='utf-8').read()
    if text.count(old) != count:
        raise SystemExit(f'{name}: {old[:60]!r} found {text.count(old)} times, not {count}')
    open(path, 'w', encoding='utf-8', newline='').write(text.replace(old, new))


# ---- imgui.h: the flag, the record buffer and the hooks ----
edit('imgui.h', '''    ImDrawListFlags_AllowVtxOffset          = 1 << 3,  // Can emit 'VtxOffset > 0' to allow large meshes. Set when 'ImGuiBackendFlags_RendererHasVtxOffset' is enabled.
};''', '''    ImDrawListFlags_AllowVtxOffset          = 1 << 3,  // Can emit 'VtxOffset > 0' to allow large meshes. Set when 'ImGuiBackendFlags_RendererHasVtxOffset' is enabled.
    ImDrawListFlags_Deferred                = 1 << 4,  // ShaderEmu: text, lines and filled shapes are handed to GImDrawListDefer, which may draw them later (a backend's)
};

// ShaderEmu (linux/imgui/imgui.patch, docs/apps/imgui.md "Drawn later"): a backend that draws an ImDrawList's text,
// lines and filled shapes later, elsewhere, by these same functions on a list of its own. Each hook answers true
// when it took the primitive (it then put what stands for it in the list), false when the list is to draw it now.
// A list draws through them only with ImDrawListFlags_Deferred, which NewFrame() gives every list when Text is set.
struct ImDrawListDefer
{
    bool (*Text)(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, ImU32 col, const ImVec4& clip_rect, const char* text_begin, const char* text_end, float wrap_width, bool cpu_fine_clip);
    bool (*Polyline)(ImDrawList* list, const ImVec2* points, int points_count, ImU32 col, ImDrawFlags flags, float thickness);
    bool (*Line)(ImDrawList* list, float x1, float y1, float x2, float y2, ImU32 col, float thickness);   // AddLine's two points, a half pixel on
    // PlotLines()'s segments: the values as the getter gives them (or `array` at `stride`, Plot_ArrayGetter's), the scale
    // (FLT_MAX: from the values), and the rest as PlotEx() has worked it out; the same AddLine() calls made later
    bool (*PlotLines)(ImDrawList* list, float (*values_getter)(void* data, int idx), void* data, const float* array, int stride, int values_count,
                      int values_offset, int res_w, int item_count, float scale_min, float scale_max, const ImVec2& min, const ImVec2& max,
                      ImU32 col_base, ImU32 col_hovered, int idx_hovered);
    bool (*ConvexPolyFilled)(ImDrawList* list, const ImVec2* points, int points_count, ImU32 col);
};
extern IMGUI_API ImDrawListDefer GImDrawListDefer;''')
edit('imgui.h', '''    const char*             _OwnerName;         // Pointer to owner window's name for debugging
''', '''    const char*             _OwnerName;         // Pointer to owner window's name for debugging
    ImVector<unsigned int>  _Deferred;          // [Internal] ShaderEmu: what GImDrawListDefer's hooks keep of the primitives they took
''')

# ---- imgui_draw.cpp: the hooks called, the buffer cleared ----
edit('imgui_draw.cpp', '''    _Path.resize(0);
    _Splitter.Clear();
    CmdBuffer.push_back(ImDrawCmd());
    _FringeScale = 1.0f;
}''', '''    _Path.resize(0);
    _Splitter.Clear();
    _Deferred.resize(0);
    CmdBuffer.push_back(ImDrawCmd());
    _FringeScale = 1.0f;
}

ImDrawListDefer GImDrawListDefer;''')
edit('imgui_draw.cpp', '''    _Path.clear();
    _Splitter.ClearFreeMemory();
}''', '''    _Path.clear();
    _Splitter.ClearFreeMemory();
    _Deferred.clear();
}''')
edit('imgui_draw.cpp', '''    if (points_count < 2 || (col & IM_COL32_A_MASK) == 0)
        return;

    const bool closed = (flags & ImDrawFlags_Closed) != 0;''', '''    if (points_count < 2 || (col & IM_COL32_A_MASK) == 0)
        return;
    if ((Flags & ImDrawListFlags_Deferred) && GImDrawListDefer.Polyline(this, points, points_count, col, flags, thickness))
        return;

    const bool closed = (flags & ImDrawFlags_Closed) != 0;''')
edit('imgui_draw.cpp', '''    if (points_count < 3 || (col & IM_COL32_A_MASK) == 0)
        return;

    const ImVec2 uv = _Data->TexUvWhitePixel;

    if (Flags & ImDrawListFlags_AntiAliasedFill)
    {''', '''    if (points_count < 3 || (col & IM_COL32_A_MASK) == 0)
        return;
    if ((Flags & ImDrawListFlags_Deferred) && GImDrawListDefer.ConvexPolyFilled(this, points, points_count, col))
        return;

    const ImVec2 uv = _Data->TexUvWhitePixel;

    if (Flags & ImDrawListFlags_AntiAliasedFill)
    {''')
edit('imgui_draw.cpp', '''        clip_rect.w = ImMin(clip_rect.w, cpu_fine_clip_rect->w);
    }
    font->RenderText(this, font_size, pos, col, clip_rect, text_begin, text_end, wrap_width, cpu_fine_clip_rect != NULL);''', '''        clip_rect.w = ImMin(clip_rect.w, cpu_fine_clip_rect->w);
    }
    if ((Flags & ImDrawListFlags_Deferred) && GImDrawListDefer.Text(this, font, font_size, pos, col, clip_rect, text_begin, text_end, wrap_width, cpu_fine_clip_rect != NULL))
        return;
    font->RenderText(this, font_size, pos, col, clip_rect, text_begin, text_end, wrap_width, cpu_fine_clip_rect != NULL);''')
edit('imgui_draw.cpp', '''    if ((col & IM_COL32_A_MASK) == 0)
        return;
    PathLineTo(p1 + ImVec2(0.5f, 0.5f));
    PathLineTo(p2 + ImVec2(0.5f, 0.5f));
    PathStroke(col, 0, thickness);''', '''    if ((col & IM_COL32_A_MASK) == 0)
        return;
    if ((Flags & ImDrawListFlags_Deferred) && GImDrawListDefer.Line(this, p1.x + 0.5f, p1.y + 0.5f, p2.x + 0.5f, p2.y + 0.5f, col, thickness))
        return;
    PathLineTo(p1 + ImVec2(0.5f, 0.5f));
    PathLineTo(p2 + ImVec2(0.5f, 0.5f));
    PathStroke(col, 0, thickness);''')
# a command (no callback's) keeps in UserCallbackData and UserCallbackDataSize what the hooks said of the records in it
# (the work of making them and the corners they may make, docs/apps/imgui.md): two commands made one add them
edit('imgui_draw.cpp', '''    if (ImDrawCmd_HeaderCompare(curr_cmd, prev_cmd) == 0 && ImDrawCmd_AreSequentialIdxOffset(prev_cmd, curr_cmd) && curr_cmd->UserCallback == NULL && prev_cmd->UserCallback == NULL)
    {
        prev_cmd->ElemCount += curr_cmd->ElemCount;''', '''    if (ImDrawCmd_HeaderCompare(curr_cmd, prev_cmd) == 0 && ImDrawCmd_AreSequentialIdxOffset(prev_cmd, curr_cmd) && curr_cmd->UserCallback == NULL && prev_cmd->UserCallback == NULL)
    {
        prev_cmd->ElemCount += curr_cmd->ElemCount;
        prev_cmd->UserCallbackData = (char*)prev_cmd->UserCallbackData + (size_t)curr_cmd->UserCallbackData;
        prev_cmd->UserCallbackDataSize += curr_cmd->UserCallbackDataSize;''')
edit('imgui_draw.cpp', '''                last_cmd->ElemCount += next_cmd->ElemCount;
                idx_offset += next_cmd->ElemCount;''', '''                last_cmd->ElemCount += next_cmd->ElemCount;
                last_cmd->UserCallbackData = (char*)last_cmd->UserCallbackData + (size_t)next_cmd->UserCallbackData;
                last_cmd->UserCallbackDataSize += next_cmd->UserCallbackDataSize;
                idx_offset += next_cmd->ElemCount;''')
# a rounded picture's vertices are given their texture coordinates after they are made: made now
edit('imgui_draw.cpp', '''    int vert_start_idx = VtxBuffer.Size;
    PathRect(p_min, p_max, rounding, flags);
    PathFillConvex(col);
    int vert_end_idx = VtxBuffer.Size;''', '''    const ImDrawListFlags deferred = Flags & ImDrawListFlags_Deferred;
    Flags &= ~ImDrawListFlags_Deferred;
    int vert_start_idx = VtxBuffer.Size;
    PathRect(p_min, p_max, rounding, flags);
    PathFillConvex(col);
    int vert_end_idx = VtxBuffer.Size;
    Flags |= deferred;''')

# ---- imgui.cpp: a window's first place in floats, not in the machine's soft doubles (two conversions
# and four sums a window a frame); the loss it records is then none, which only a scroll of more than
# 2^24 pixels would have had ----
edit('imgui.cpp', '''        double start_pos_highp_x = (double)window->Pos.x + window->WindowPadding.x - (double)window->Scroll.x + window->DecoOuterSizeX1 + window->DC.ColumnsOffset.x;
        double start_pos_highp_y = (double)window->Pos.y + window->WindowPadding.y - (double)window->Scroll.y + window->DecoOuterSizeY1;''', '''        float start_pos_highp_x = window->Pos.x + window->WindowPadding.x - window->Scroll.x + window->DecoOuterSizeX1 + window->DC.ColumnsOffset.x;
        float start_pos_highp_y = window->Pos.y + window->WindowPadding.y - window->Scroll.y + window->DecoOuterSizeY1;''')

# ---- imgui.cpp: the keys' two walks of every named key (154) skipped while no key is down, held, owned or locked and
# nothing has touched one since (KeysIdle: what writes a key's state or owner clears it); a key that is up and was up,
# with no owner and no lock, is as it was in them; with no routes there is none to update ----
edit('imgui_internal.h', '''    ImGuiWindow*            HoveredWindow;                      // Window the mouse is hovering. Will typically catch mouse inputs.''',
     '''    bool                    KeysIdle;                           // ShaderEmu: no named key is down, held, owned or locked, and none has been touched since
    ImGuiWindow*            HoveredWindow;                      // Window the mouse is hovering. Will typically catch mouse inputs.''')
edit('imgui.cpp', '''    HoveredWindow = NULL;''', '''    KeysIdle = false;
    HoveredWindow = NULL;''')
edit('imgui.cpp', '''    ImGuiKeyData* key_data = ImGui::GetKeyData(key);
    key_data->Down = v;
    key_data->AnalogValue = analog_value;''', '''    ImGuiKeyData* key_data = ImGui::GetKeyData(key);
    if (v)
        GImGui->KeysIdle = false;
    key_data->Down = v;
    key_data->AnalogValue = analog_value;''')
edit('imgui.cpp', '''            key_data->Down = e->Key.Down;
            key_data->AnalogValue = e->Key.AnalogValue;
            key_changed = true;''', '''            key_data->Down = e->Key.Down;
            key_data->AnalogValue = e->Key.AnalogValue;
            key_changed = true;
            g.KeysIdle = false;''')
edit('imgui.cpp', '''    ImGuiKeyOwnerData* owner_data = GetKeyOwnerData(&g, key);
    owner_data->OwnerCurr = owner_data->OwnerNext = owner_id;''', '''    ImGuiKeyOwnerData* owner_data = GetKeyOwnerData(&g, key);
    owner_data->OwnerCurr = owner_data->OwnerNext = owner_id;
    g.KeysIdle = false;''')
edit('imgui.cpp', '''                    owner_data->OwnerCurr = routing_entry->RoutingCurr;''', '''                    owner_data->OwnerCurr = routing_entry->RoutingCurr;
                    g.KeysIdle = false;''')
edit('imgui.cpp', '''    // Clear gamepad data if disabled
    if ((io.BackendFlags & ImGuiBackendFlags_HasGamepad) == 0)''', '''    // Clear gamepad data if disabled
    const bool keys_idle = g.KeysIdle;
    bool keys_busy = false;
    if (!keys_idle && (io.BackendFlags & ImGuiBackendFlags_HasGamepad) == 0)''')
edit('imgui.cpp', '''    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
    {
        ImGuiKeyData* key_data = &io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
        key_data->DownDurationPrev = key_data->DownDuration;''', '''    for (int key = ImGuiKey_NamedKey_BEGIN; !keys_idle && key < ImGuiKey_NamedKey_END; key++)
    {
        ImGuiKeyData* key_data = &io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
        if (!key_data->Down && key_data->DownDuration < 0.0f && key_data->DownDurationPrev < 0.0f)
            continue;
        keys_busy = true;
        key_data->DownDurationPrev = key_data->DownDuration;''')
edit('imgui.cpp', '''    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = (ImGuiKey)(key + 1))
    {
        ImGuiKeyData* key_data = &io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
        ImGuiKeyOwnerData* owner_data = &g.KeysOwnerData[key - ImGuiKey_NamedKey_BEGIN];
        owner_data->OwnerCurr = owner_data->OwnerNext;''', '''    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; !keys_idle && key < ImGuiKey_NamedKey_END; key = (ImGuiKey)(key + 1))
    {
        ImGuiKeyData* key_data = &io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
        ImGuiKeyOwnerData* owner_data = &g.KeysOwnerData[key - ImGuiKey_NamedKey_BEGIN];
        if (owner_data->OwnerCurr == ImGuiKeyOwner_NoOwner && owner_data->OwnerNext == ImGuiKeyOwner_NoOwner && !owner_data->LockThisFrame && !owner_data->LockUntilRelease)
            continue;
        keys_busy = true;
        owner_data->OwnerCurr = owner_data->OwnerNext;''')
edit('imgui.cpp', '''    // Update key routing (for e.g. shortcuts)
    UpdateKeyRoutingTable(&g.KeysRoutingTable);''', '''    g.KeysIdle = !keys_busy;

    // Update key routing (for e.g. shortcuts)
    UpdateKeyRoutingTable(&g.KeysRoutingTable);''')
edit('imgui.cpp', '''    ImGuiContext& g = *GImGui;
    rt->EntriesNext.resize(0);
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN;''', '''    ImGuiContext& g = *GImGui;
    rt->EntriesNext.resize(0);
    if (rt->Entries.Size == 0)
        return;     // (every key's Index is -1 then)
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN;''')

# ---- imgui.cpp: a window's resize grip or border that the pointer is not over and that is not held is neither: only what
# ItemAdd() would leave of it is made (ButtonBehavior() would find nothing) ----
edit('imgui.cpp', '''// Handle resize for: Resize Grips, Borders, Gamepad
// Return true when using auto-fit (double-click on resize grip)''', '''// ShaderEmu: true when a resize grip or border of the current window can be neither hovered nor held (nor anything
// else of an item's: not the active, navigated or located one, and no next item's data but flags to clear); then
// what ItemAdd() leaves of it is made here, and ItemAdd() and ButtonBehavior() are not called.
static bool ResizeHandleQuiet(const ImRect& r, ImGuiID id)
{
    ImGuiContext& g = *GImGui;
    if (id == g.ActiveId || id == g.ActiveIdPreviousFrame || id == g.NavId || id == g.NavActivateId || id == g.DebugLocateId || g.NextItemData.HasFlags)
        return false;
    const ImVec2 m = g.IO.MousePos, pad = g.Style.TouchExtraPadding;
    if (m.x >= r.Min.x - pad.x && m.y >= r.Min.y - pad.y && m.x < r.Max.x + pad.x && m.y < r.Max.y + pad.y)
        return false;
    g.LastItemData.ID = id;
    g.LastItemData.Rect = g.LastItemData.NavRect = r;
    g.LastItemData.ItemFlags = g.CurrentItemFlags | g.NextItemData.ItemFlags | ImGuiItemFlags_NoNav;
    g.LastItemData.StatusFlags = r.Overlaps(g.CurrentWindow->ClipRect) ? ImGuiItemStatusFlags_Visible : ImGuiItemStatusFlags_None;
    g.NextItemData.ItemFlags = ImGuiItemFlags_None;
    return true;
}

// Handle resize for: Resize Grips, Borders, Gamepad
// Return true when using auto-fit (double-click on resize grip)''')
edit('imgui.cpp', '''        bool hovered, held;
        ImRect resize_rect(corner - def.InnerDir * grip_hover_outer_size, corner + def.InnerDir * grip_hover_inner_size);
        if (resize_rect.Min.x > resize_rect.Max.x) ImSwap(resize_rect.Min.x, resize_rect.Max.x);
        if (resize_rect.Min.y > resize_rect.Max.y) ImSwap(resize_rect.Min.y, resize_rect.Max.y);
        ImGuiID resize_grip_id = window->GetID(resize_grip_n); // == GetWindowResizeCornerID()
        ItemAdd(resize_rect, resize_grip_id, NULL, ImGuiItemFlags_NoNav);
        ButtonBehavior(resize_rect, resize_grip_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren | ImGuiButtonFlags_NoNavFocus);''', '''        bool hovered = false, held = false;
        ImRect resize_rect(corner - def.InnerDir * grip_hover_outer_size, corner + def.InnerDir * grip_hover_inner_size);
        if (resize_rect.Min.x > resize_rect.Max.x) ImSwap(resize_rect.Min.x, resize_rect.Max.x);
        if (resize_rect.Min.y > resize_rect.Max.y) ImSwap(resize_rect.Min.y, resize_rect.Max.y);
        ImGuiID resize_grip_id = window->GetID(resize_grip_n); // == GetWindowResizeCornerID()
        if (!ResizeHandleQuiet(resize_rect, resize_grip_id))
        {
        ItemAdd(resize_rect, resize_grip_id, NULL, ImGuiItemFlags_NoNav);
        ButtonBehavior(resize_rect, resize_grip_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren | ImGuiButtonFlags_NoNavFocus);
        }''')
edit('imgui.cpp', '''        bool hovered, held;
        ImRect border_rect = GetResizeBorderRect(window, border_n, grip_hover_inner_size, WINDOWS_HOVER_PADDING);
        ImGuiID border_id = window->GetID(border_n + 4); // == GetWindowResizeBorderID()
        ItemAdd(border_rect, border_id, NULL, ImGuiItemFlags_NoNav);
        ButtonBehavior(border_rect, border_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren | ImGuiButtonFlags_NoNavFocus);''', '''        bool hovered = false, held = false;
        ImRect border_rect = GetResizeBorderRect(window, border_n, grip_hover_inner_size, WINDOWS_HOVER_PADDING);
        ImGuiID border_id = window->GetID(border_n + 4); // == GetWindowResizeBorderID()
        if (!ResizeHandleQuiet(border_rect, border_id))
        {
        ItemAdd(border_rect, border_id, NULL, ImGuiItemFlags_NoNav);
        ButtonBehavior(border_rect, border_id, &hovered, &held, ImGuiButtonFlags_FlattenChildren | ImGuiButtonFlags_NoNavFocus);
        }''')

# ---- imgui.cpp: the fallback window ("Debug##Default"), which NewFrame() begins every frame and shows only when
# something was drawn into it: begun with no title bar, background, resizing or scroll bars after a frame with
# nothing in it (it is not drawn then), as it is after one with something ----
edit('imgui_internal.h', '''    ImGuiWindow*            HoveredWindow;                      // Window the mouse is hovering. Will typically catch mouse inputs.''',
     '''    ImGuiWindow*            FallbackWindowBefore;               // ShaderEmu: the fallback window of the frame before
    ImGuiWindow*            HoveredWindow;                      // Window the mouse is hovering. Will typically catch mouse inputs.''')
edit('imgui.cpp', '''    HoveredWindow = NULL;''', '''    FallbackWindowBefore = NULL;
    HoveredWindow = NULL;''')
edit('imgui.cpp', '''    SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
    Begin("Debug##Default");
    IM_ASSERT(g.CurrentWindow->IsFallbackWindow == true);''', '''    SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
    const bool fallback_bare = g.FallbackWindowBefore && !g.FallbackWindowBefore->WasActive;
    Begin("Debug##Default", NULL, fallback_bare ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                                                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse : 0);
    g.FallbackWindowBefore = g.CurrentWindow;
    IM_ASSERT(g.CurrentWindow->IsFallbackWindow == true);''')

# ---- imgui_draw.cpp: a text's width, a run of plain characters at a time when it is of the font's own size and has no
# limit (the same sums: each its advance times 1) ----
edit('imgui_draw.cpp', '''    const bool word_wrap_enabled = (wrap_width > 0.0f);
    const char* word_wrap_eol = NULL;

    const char* s = text_begin;
    while (s < text_end)
    {
        if (word_wrap_enabled)
        {
            // Calculate how far we can render. Requires two passes on the string data but keeps the code simple and not intrusive for what's essentially an uncommon feature.
            if (!word_wrap_eol)
                word_wrap_eol = CalcWordWrapPositionA(scale, s, text_end, wrap_width - line_width);''', '''    const bool word_wrap_enabled = (wrap_width > 0.0f);
    const char* word_wrap_eol = NULL;
    const float* plain = !word_wrap_enabled && scale == 1.0f && max_width == FLT_MAX && IndexAdvanceX.Size >= 0x80 ? IndexAdvanceX.Data : NULL;

    const char* s = text_begin;
    while (s < text_end)
    {
        if (plain)
        {
            unsigned int c;
            while (s < text_end && (c = (unsigned char)*s) >= 32 && c < 0x80)
                line_width += plain[c], s++;
            if (s >= text_end)
                break;
        }
        if (word_wrap_enabled)
        {
            // Calculate how far we can render. Requires two passes on the string data but keeps the code simple and not intrusive for what's essentially an uncommon feature.
            if (!word_wrap_eol)
                word_wrap_eol = CalcWordWrapPositionA(scale, s, text_end, wrap_width - line_width);''')

# ---- imgui.cpp: a style colour's last words kept with what they were made of, bit for bit (some 35 a frame with the
# demo window, four floats' conversion each) ----
edit('imgui.cpp', '''ImU32 ImGui::GetColorU32(ImGuiCol idx, float alpha_mul)
{
    ImGuiStyle& style = GImGui->Style;
    ImVec4 c = style.Colors[idx];
    c.w *= style.Alpha * alpha_mul;
    return ColorConvertFloat4ToU32(c);
}''', '''ImU32 ImGui::GetColorU32(ImGuiCol idx, float alpha_mul)
{
    ImGuiStyle& style = GImGui->Style;
    ImVec4 c = style.Colors[idx];
    c.w *= style.Alpha * alpha_mul;
    // ShaderEmu: the colour as it was made last, when its four floats are the same bits
    static struct { ImU32 bits[4], out; } made[ImGuiCol_COUNT];
    ImU32 bits[4];
    memcpy(bits, &c, sizeof bits);
    if ((unsigned)idx < ImGuiCol_COUNT)
    {
        if (made[idx].bits[0] == bits[0] && made[idx].bits[1] == bits[1] && made[idx].bits[2] == bits[2] && made[idx].bits[3] == bits[3] && made[idx].out)
            return made[idx].out;
        memcpy(made[idx].bits, bits, sizeof bits);
        return made[idx].out = ColorConvertFloat4ToU32(c);
    }
    return ColorConvertFloat4ToU32(c);
}''')

# ---- imgui.cpp: every list of a frame draws through the hooks when a backend set them ----
edit('imgui.cpp', '''    if (g.IO.BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset)
        g.DrawListSharedData.InitialFlags |= ImDrawListFlags_AllowVtxOffset;
}''', '''    if (g.IO.BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset)
        g.DrawListSharedData.InitialFlags |= ImDrawListFlags_AllowVtxOffset;
    if (GImDrawListDefer.Text)
        g.DrawListSharedData.InitialFlags |= ImDrawListFlags_Deferred;
}''')

# ---- imgui_widgets.cpp: a plot's lines made later; the scale from its values found there too ----
edit('imgui_widgets.cpp', '''int ImGui::PlotEx(ImGuiPlotType plot_type, const char* label, float (*values_getter)(void* data, int idx), void* data, int values_count, int values_offset, const char* overlay_text, float scale_min, float scale_max, const ImVec2& size_arg)
{''', '''struct ImGuiPlotArrayGetterData;
static float Plot_ArrayGetter(void* data, int idx);

int ImGui::PlotEx(ImGuiPlotType plot_type, const char* label, float (*values_getter)(void* data, int idx), void* data, int values_count, int values_offset, const char* overlay_text, float scale_min, float scale_max, const ImVec2& size_arg)
{''')
edit('imgui_widgets.cpp', '''    // Determine scale from values if not specified
    if (scale_min == FLT_MAX || scale_max == FLT_MAX)
    {''', '''    // ShaderEmu: a plot's lines made later (GImDrawListDefer.PlotLines), with the scale
    const bool later = plot_type == ImGuiPlotType_Lines && values_count >= 2 && (window->DrawList->Flags & ImDrawListFlags_Deferred) && GImDrawListDefer.PlotLines;

    // Determine scale from values if not specified
    if (!later && (scale_min == FLT_MAX || scale_max == FLT_MAX))
    {''')
edit('imgui_widgets.cpp', '''            idx_hovered = v_idx;
        }

        const float t_step = 1.0f / (float)res_w;''', '''            idx_hovered = v_idx;
        }

        if (later)
        {
            const ImU32 col_base = GetColorU32(ImGuiCol_PlotLines), col_hovered = GetColorU32(ImGuiCol_PlotLinesHovered);
            const bool array = values_getter == &Plot_ArrayGetter;
            const float* values = array ? *(const float* const*)data : NULL;
            const int stride = array ? ((const int*)data)[1] : 0;
            if (GImDrawListDefer.PlotLines(window->DrawList, values_getter, data, values, stride, values_count, values_offset, res_w, item_count,
                                           scale_min, scale_max, inner_bb.Min, inner_bb.Max, col_base, col_hovered, idx_hovered))
                values_count = 0;   // (drawn: the loop below has nothing to do)
            else if (scale_min == FLT_MAX || scale_max == FLT_MAX)
            {
                float v_min = FLT_MAX;
                float v_max = -FLT_MAX;
                for (int i = 0; i < values_count; i++)
                {
                    const float v = values_getter(data, i);
                    if (v != v) // Ignore NaN values
                        continue;
                    v_min = ImMin(v_min, v);
                    v_max = ImMax(v_max, v);
                }
                if (scale_min == FLT_MAX)
                    scale_min = v_min;
                if (scale_max == FLT_MAX)
                    scale_max = v_max;
            }
        }
    }
    if (values_count >= values_count_min)
    {
        int res_w = ImMin((int)frame_size.x, values_count) + ((plot_type == ImGuiPlotType_Lines) ? -1 : 0);
        int item_count = values_count + ((plot_type == ImGuiPlotType_Lines) ? -1 : 0);
        const float t_step = 1.0f / (float)res_w;''')

# ---- the hue wheel's and angled headers' vertices are changed after they are made: made now ----
edit('imgui_widgets.cpp', '''            const int vert_start_idx = draw_list->VtxBuffer.Size;
            draw_list->PathArcTo(wheel_center, (wheel_r_inner + wheel_r_outer)*0.5f, a0, a1, segment_per_arc);
            draw_list->PathStroke(col_white, 0, wheel_thickness);
            const int vert_end_idx = draw_list->VtxBuffer.Size;''', '''            const ImDrawListFlags deferred = draw_list->Flags & ImDrawListFlags_Deferred;
            draw_list->Flags &= ~ImDrawListFlags_Deferred;
            const int vert_start_idx = draw_list->VtxBuffer.Size;
            draw_list->PathArcTo(wheel_center, (wheel_r_inner + wheel_r_outer)*0.5f, a0, a1, segment_per_arc);
            draw_list->PathStroke(col_white, 0, wheel_thickness);
            const int vert_end_idx = draw_list->VtxBuffer.Size;
            draw_list->Flags |= deferred;''')
edit('imgui_tables.cpp', '''                    int vtx_idx_begin = draw_list->_VtxCurrentIdx;
                    PushStyleColor(ImGuiCol_Text, request->TextColor);
                    RenderTextEllipsis(draw_list, clip_r.Min, clip_r.Max, clip_r.Max.x, clip_r.Max.x, label_name, label_name_eol, &label_size);
                    PopStyleColor();
                    int vtx_idx_end = draw_list->_VtxCurrentIdx;''', '''                    const ImDrawListFlags deferred = draw_list->Flags & ImDrawListFlags_Deferred;
                    draw_list->Flags &= ~ImDrawListFlags_Deferred;
                    int vtx_idx_begin = draw_list->_VtxCurrentIdx;
                    PushStyleColor(ImGuiCol_Text, request->TextColor);
                    RenderTextEllipsis(draw_list, clip_r.Min, clip_r.Max, clip_r.Max.x, clip_r.Max.x, label_name, label_name_eol, &label_size);
                    PopStyleColor();
                    int vtx_idx_end = draw_list->_VtxCurrentIdx;
                    draw_list->Flags |= deferred;''')
print('edits made')
