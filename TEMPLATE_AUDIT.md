# Template Binding Audit: note_card_row

## Audit Results

### ✅ MATCHING BINDINGS

| Widget ID | Blueprint Type | C Binding | Status |
|-----------|---------------|-----------|---------|
| root | Box | ✓ | OK |
| btn_avatar | Button | ✓ | OK |
| btn_display_name | Button | ✓ | OK |
| btn_menu | Button | ✓ | OK |
| btn_reply | Button | ✓ | OK |
| btn_repost | Button | ✓ | OK |
| btn_like | Button | ✓ | OK |
| lbl_like_count | Label | ✓ | OK |
| btn_zap | Button | ✓ | OK |
| lbl_zap_count | Label | ✓ | OK |
| btn_bookmark | Button | ✓ | OK |
| btn_thread | Button | ✓ | OK |
| reply_indicator_box | Box | ✓ | OK |
| reply_indicator_label | Label | ✓ | OK |
| reply_count_box | Box | ✓ | OK |
| reply_count_label | Label | ✓ | OK |
| avatar_box | Overlay | ✓ | OK |
| avatar_initials | Label | ✓ | OK |
| lbl_display | Label | ✓ | OK |
| lbl_handle | Label | ✓ | OK |
| lbl_nip05_separator | Label | ✓ | OK |
| lbl_nip05 | Label | ✓ | OK |
| lbl_timestamp_separator | Label | ✓ | OK |
| lbl_timestamp | Label | ✓ | OK |
| content_label | Label | ✓ | OK |
| media_box | Box | ✓ | OK |
| embed_box | Frame | ✓ | OK |
| og_preview_container | Box | ✓ | OK |
| actions_box | Box | ✓ | OK |
| subject_label | Label | ✓ | OK |
| sensitive_content_overlay | Overlay | ✓ | OK |
| sensitive_warning_box | Box | ✓ | OK |
| sensitive_warning_label | Label | ✓ | OK |
| btn_show_sensitive | Button | ✓ | OK |
| hashtags_box | FlowBox | ✓ | OK |
| labels_box | FlowBox | ✓ | OK |
| external_ids_box | FlowBox | ✓ | OK |

### ⚠️ TYPE MISMATCH (FIXED)

| Widget ID | Blueprint Type | C Code Expected | Issue | Status |
|-----------|---------------|-----------------|-------|---------|
| avatar_image | **Picture** | GtkImage (wrong) | C code was calling gtk_image_clear() on a GtkPicture | **FIXED** - Now uses gtk_picture_set_paintable() |

### ❌ MISSING FROM BLUEPRINT (C code binds but not in template)

**None found** - All C bindings have corresponding Blueprint definitions.

### 📝 WIDGETS IN BLUEPRINT BUT NOT BOUND IN C

These widgets exist in the Blueprint but are NOT bound as template children in C code. They may be accessed dynamically or not used:

| Widget ID | Type | Location | Notes |
|-----------|------|----------|-------|
| reply_indicator_icon | Image | Line 30 | Inside reply_indicator_box |
| header_info | Box | Line 116 | Container for header content |
| header_top | Box | Line 123 | Top row of header |
| header_bottom | Box | Line 188 | Bottom row of header |
| lbl_kind_badge | Label | Line 203 | Kind badge label |
| lbl_relay | Label | Line 213 | Relay label |
| main_content | Box | Line 51 | Main content container |
| content_column | Box | Line 239 | Content column container |
| sensitive_warning_icon | Image | Line 257 | Warning icon in sensitive overlay |
| like_button_box | Box | Line 430 | Container for like button content |
| like_icon | Image | Line 434 | Like button icon |
| zap_button_box | Box | Line 459 | Container for zap button content |
| zap_icon | Image | Line 463 | Zap button icon |
| btn_link | Button | Line 483 | Copy link button |
| reply_count_icon | Image | Line 541 | Reply count icon |
| lbl_reply_count | Label | Line 381 | Reply count in reply button |
| lbl_repost_count | Label | Line 410 | Repost count in repost button |

### 🔍 ANALYSIS

**Critical Issues Found:**
1. **avatar_image type mismatch** - Blueprint defines `Picture` but C code treated it as `Image`. This has been **FIXED**.

**Non-Critical (Unbound widgets):**
- Many container boxes and icons are not bound because they're only used for layout/styling
- Action count labels inside buttons (lbl_reply_count, lbl_repost_count) are not bound - these may need to be added if the code tries to update them
- btn_link is not bound but may be accessed dynamically

**Recommendations:**
1. ✅ **DONE** - Fixed avatar_image type mismatch
2. Check if lbl_reply_count and lbl_repost_count need to be bound (if code updates reply/repost counts)
3. Check if btn_link needs to be bound (if copy link functionality is implemented)
4. Consider binding lbl_kind_badge and lbl_relay if they're used for displaying kind/relay info

**Potential Crash Sources:**
- The avatar_image type mismatch could have caused memory corruption when the C code tried to call GtkImage methods on a GtkPicture widget
- Any code that tries to access unbound widgets (like lbl_reply_count) would get NULL pointers

## Summary

**Total Blueprint widgets:** ~50+
**Bound in C code:** 38
**Type mismatches:** 1 (FIXED)
**Missing bindings:** 0 (all C bindings exist in Blueprint)
**Unbound but present:** ~15 (mostly layout containers and icons)

The main issue was the avatar_image type mismatch which has been corrected. The unbound widgets are mostly layout containers that don't need to be accessed from C code.
