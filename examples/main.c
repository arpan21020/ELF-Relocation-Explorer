extern int add(int, int);

static int base = 1;
static const char greeting[] = "hello";
static const char name[] = "linker";

int main(void) {
    return add(greeting[base], name[base]);
}
