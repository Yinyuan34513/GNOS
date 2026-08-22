#include "ulib.h"
int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;
    int n = 0;
    for (; envp && envp[n]; n++) {
        print("ENV[");
        printn(n);
        print("]=");
        print(envp[n]);
        print("\n");
    }
    print("env total ");
    printn(n);
    print("\n");
    return 0;
}
