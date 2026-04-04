#include <iostream>
#include "Krpccontroller.h"
int main() {
    Krpccontroller c;
    c.SetFailed("test");
    std::cout << "failed: " << c.Failed() << std::endl;
}
