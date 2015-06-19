from distutils.core import setup, Extension

setup(name="measure_loc",
        version="1.0",
        description="measures loc",
        ext_modules=[
            Extension("measure_loc_ext", sources = ["measure_loc.cpp"]),
        ],
    )
