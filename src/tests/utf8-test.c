#include <stdio.h>
#include <assert.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

int main(int argc, char *argv[]) {
    char *c;

    assert(pa_utf8_valid("hallo"));
    assert(pa_utf8_valid("hallo\n"));
    assert(!pa_utf8_valid("h�pfburg\n"));
    assert(pa_utf8_valid("hallo\n"));
    assert(pa_utf8_valid("hüpfburg\n"));

    printf("LATIN1: %s\n", c = pa_utf8_filter("h�pfburg"));
    pa_xfree(c);
    printf("UTF8: %sx\n", c = pa_utf8_filter("hüpfburg"));
    pa_xfree(c);
    printf("LATIN1: %sx\n", c = pa_utf8_filter("�xkn�rzm�rzelt�rsz߳�dsjkfh"));
    pa_xfree(c);

    return 0;
}
