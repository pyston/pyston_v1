from distutils.core import setup, Extension

setup(name="test",
        version="1.0",
        description="test",
        ext_modules=[
            Extension("basic_test", sources = ["basic_test.c"]),
            Extension("descr_test", sources = ["descr_test.c"]),
        ],
    )
