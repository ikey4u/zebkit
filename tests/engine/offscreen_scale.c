/* Independent frame pixels, CSS layout and DPR across two live views. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zebkit/offscreen.h>

struct picture { int count, width, height; float scale; };
static int failures;
static void check(int ok, const char *label) {
    printf("%s %s\n",ok ? "ok" : "FAIL",label);
    fflush(stdout);
    if (!ok) ++failures;
}
static double now(void) {
    struct timespec value; clock_gettime(CLOCK_MONOTONIC,&value);
    return value.tv_sec+value.tv_nsec/1e9;
}
static void step(void) {
    struct timespec pause={0,2000000};
    if (!zebkit_pump()) ++failures;
    nanosleep(&pause,NULL);
}
static void frame(zebkit_view *view,const zebkit_frame *value,void *data) {
    (void)view;
    struct picture *picture=data;
    if (value->layer==ZEBKIT_LAYER_VIEW) {
        ++picture->count; picture->width=value->width;
        picture->height=value->height; picture->scale=value->scale;
    }
    zebkit_frame_release(value);
}
struct result { int done; char value[256]; };
static void evaluated(zebkit_view *view,const char *json,const char *error,void *data) {
    (void)view;
    struct result *result=data;
    snprintf(result->value,sizeof(result->value),"%s",error ? error : json ? json : "null");
    result->done=1;
}
static void read_result(zebkit_view *view,const char *script,struct result *result) {
    zebkit_view_evaluate(view,script,evaluated,result);
    double end=now()+10;
    while (!result->done && now()<end) step();
}
static void query(zebkit_view *view,const char *script,const char *expected,const char *label) {
    struct result result={0};
    read_result(view,script,&result);
    if (!result.done || strcmp(result.value,expected)) fprintf(stderr,"identity %s: %s expected %s\n",label,result.value,expected);
    check(result.done && !strcmp(result.value,expected),label);
}
static void identity(zebkit_view *view,const char *expected,const char *label) {
    query(view,"[devicePixelRatio,innerWidth,innerHeight]",expected,label);
}
static void click(zebkit_view *view,const char *label) {
    zebkit_view_set_focus(view,true);
    zebkit_view_send_mouse(view,ZEBKIT_MOUSE_MOVE,40,35,ZEBKIT_BUTTON_NONE,0,0);
    zebkit_view_send_mouse(view,ZEBKIT_MOUSE_DOWN,40,35,ZEBKIT_BUTTON_LEFT,1,0);
    zebkit_view_send_mouse(view,ZEBKIT_MOUSE_UP,40,35,ZEBKIT_BUTTON_LEFT,1,0);
    for (int i=0;i<20;++i) step();
    query(view,"clicked","1",label);
}
static void wait_picture(struct picture *picture,int width,int height) {
    double end=now()+10;
    while ((!picture->count || picture->width!=width || picture->height!=height) && now()<end) step();
}
int main(int argc,char **argv) {
    if (zebkit_init(argc,argv)) return 2;
    const char *page="data:text/html,<style>body{margin:0;background:rgb(12,34,56)}</style><script>window.clicked=0</script><button style='position:absolute;left:20px;top:20px;width:60px;height:30px' onclick='++window.clicked'>click</button>";
    zebkit_view *normal=zebkit_view_new(page,200,100);
    if (!normal) return 2;
    struct result native_density={0};
    read_result(normal,"devicePixelRatio",&native_density);
    char *density_end=NULL;
    double density=strtod(native_density.value,&density_end);
    check(native_density.done && density>0 && density_end && !*density_end,"normal headless view reports its original DPR");
    struct picture first={0},second={0};
    zebkit_view *a=zebkit_view_new_offscreen(page,200,100,1);
    if (!a) return 2;
    zebkit_view_on_frame(a,frame,&first);
    wait_picture(&first,200,100);
    identity(a,"[1,200,100]","first view starts at scale one");
    zebkit_view *b=zebkit_view_new_offscreen(page,200,100,2);
    if (!b) return 2;
    zebkit_view_on_frame(b,frame,&second);
    wait_picture(&second,400,200);
    zebkit_view_request_frame(a);
    for (int i=0;i<50;++i) step();
    identity(a,"[1,200,100]","second view does not change first DPR or CSS layout");
    identity(b,"[2,200,100]","second view has its own DPR");
    check(first.width==200 && first.height==100 && first.scale==1,"first frame retains its own pixel scale");
    check(second.width==400 && second.height==200 && second.scale==2,"second frame uses scale two pixels");
    query(normal,"devicePixelRatio",native_density.value,"offscreen scales preserve normal headless DPR");
    click(a,"first view receives CSS-coordinate click");
    click(b,"scale-two view receives CSS-coordinate click");
    zebkit_view_set_size(a,3000,140,1);
    wait_picture(&first,3000,140);
    identity(a,"[1,3000,140]","large first view retains its own display");
    identity(b,"[2,200,100]","large resize cannot overlap second display");
    query(normal,"devicePixelRatio",native_density.value,"large offscreen resize preserves normal headless DPR");
    zebkit_view_set_size(a,200,100,1);
    wait_picture(&first,200,100);
    zebkit_view_set_size(b,240,120,1.5f);
    wait_picture(&second,360,180);
    identity(a,"[1,200,100]","rescaling second view preserves first layout");
    identity(b,"[1.5,240,120]","second view supports independent fractional scale");
    check(second.width==360 && second.height==180 && second.scale==1.5f,"fractional frame pixels match CSS layout");
    zebkit_view_set_size(b,239,121,1.5f);
    wait_picture(&second,359,182);
    identity(b,"[1.5,239,121]","odd CSS dimensions retain fractional layout");
    check(second.width==359 && second.height==182,"fractional frame dimensions round up");
    struct picture third={0},fourth={0};
    zebkit_view *c=zebkit_view_new_offscreen(page,3600,80,.5f);
    if (!c) return 2;
    zebkit_view_on_frame(c,frame,&third);
    wait_picture(&third,1800,40);
    zebkit_view *d=zebkit_view_new_offscreen(page,200,100,2);
    if (!d) return 2;
    zebkit_view_on_frame(d,frame,&fourth);
    wait_picture(&fourth,400,200);
    identity(c,"[0.5,3600,80]","wide half-scale display stays isolated");
    identity(d,"[2,200,100]","next view cannot overlap half-scale DIP bounds");
    identity(b,"[1.5,239,121]","new displays preserve existing fractional DPR");
    zebkit_view_close(c);
    for (int i=0;i<30;++i) step();
    identity(d,"[2,200,100]","removing an earlier display preserves later scale");
    zebkit_view_close(d);
    zebkit_view_close(b);
    for (int i=0;i<50;++i) step();
    identity(a,"[1,200,100]","closing second view preserves first display");
    zebkit_view_close(a);
    zebkit_view_close(normal);
    check(zebkit_shutdown(),"offscreen scale engine shuts down");
    return failures ? 1 : 0;
}
