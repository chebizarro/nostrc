/* A Python embedding host is an executable consumer of GNostr, like the
 * scanner probe. It must link the same executable-owned core contract. */
#include <Python.h>

int main(int argc, char **argv)
{
  return Py_BytesMain(argc, argv);
}
