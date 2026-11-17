#ifndef FIPSTOP_UI_H
#define FIPSTOP_UI_H
#include "app.h"
typedef enum ft_style { FT_NORMAL, FT_DIM, FT_CYAN, FT_YELLOW, FT_GREEN, FT_RED, FT_SELECT } ft_style;
typedef struct ft_cell { wchar_t ch; unsigned char style; } ft_cell;
typedef struct ft_frame { int w,h; ft_cell *cells; } ft_frame;
typedef struct ft_rect { int x,y,w,h; } ft_rect;
bool ft_frame_init(ft_frame *f,int w,int h);
void ft_frame_free(ft_frame *f);
void ft_frame_print(const ft_frame *f,FILE *out);
void ft_put(ft_frame *f,ft_rect r,int x,int y,ft_style style,const char *text);
void ft_printf(ft_frame *f,ft_rect r,int x,int y,ft_style style,const char *fmt,...);
ft_rect ft_box(ft_frame *f,ft_rect r,const char *title,bool focused);
void ft_field(ft_frame *f,ft_rect r,int row,const char *label,const json_t *d,const char *path);
void ft_fields(ft_frame *f,ft_rect r,const json_t *d,const char *const *labels,const char *const *paths,size_t n,size_t *scroll);
void ft_aligned_fields_at(ft_frame *f,ft_rect r,const json_t *d,const char *const *labels,const char *const *paths,size_t n,size_t scroll);
void ft_aligned_fields(ft_frame *f,ft_rect r,const json_t *d,const char *const *labels,const char *const *paths,size_t n,size_t *scroll);
void ft_scrollbar(ft_frame *f,ft_rect r,size_t count,size_t selected);
void ft_detail(ft_frame *f,ft_rect r,const json_t *d,size_t *scroll,const char *title);
typedef struct ft_column { const char *label,*path; int width; } ft_column;
void ft_table_header(ft_frame *f,ft_rect r,const ft_column *cols,size_t n);
void ft_table_row(ft_frame *f,ft_rect r,int row,const ft_column *cols,size_t n,const json_t *d,bool selected);
void ft_draw(ft_frame *f,ft_app *a);
void ft_split3(ft_rect r,int first,int second,ft_rect out[3]);
json_t *ft_role_peers(const ft_app *a,const json_t *rows,const char *addr_key);
void ft_ui_node(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_peers(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_sessions(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_transports(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_tree(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_bloom(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_mmp(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_routing(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_graphs(ft_frame *f,ft_app *a,ft_rect r);
void ft_ui_gateway(ft_frame *f,ft_app *a,ft_rect r);
#endif
