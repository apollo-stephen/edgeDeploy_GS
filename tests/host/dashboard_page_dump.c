#include <stdio.h>

#include "dashboard_page.h"

int main(void)
{
    fputs(http_capture_dashboard_html(), stdout);
    return 0;
}
