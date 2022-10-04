#include <thread>
#include <assert.h>
#include <iostream>
using namespace std;
int x = 0;
void foo()
{
    while (true)
    {
        x = x + 1;
        x = x - 1;
        std::cout<<x<<std::endl;
    }
}

int main()
{
    std::thread t(foo);
    std::thread t2(foo);

    t.join();
    t2.join();
}