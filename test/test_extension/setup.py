from distutils.core import setup, Extension

test_module = Extension("test", sources = ["test.c"])

setup(name="test",
        version="1.0",
        description="test",
        ext_modules=[test_module],
        )
