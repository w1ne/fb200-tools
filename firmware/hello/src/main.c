void app_main(void)
{
    for (;;) {
        __asm volatile ("wfi");
    }
}
