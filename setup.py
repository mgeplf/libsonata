import inspect
import os
import platform
import re
import subprocess
import sys

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from setuptools.command.test import test
from distutils.version import LooseVersion


REQUIRED_NUMPY_VERSION = "numpy>=1.12.0"
MIN_CPU_CORES = 2


class lazy_dict(dict):
    """When the value associated to a key is a function, then returns
    the function call instead of the function.
    """

    def __getitem__(self, item):
        value = dict.__getitem__(self, item)
        if inspect.isfunction(value):
            return value()
        return value


def get_sphinx_command():
    """Lazy load of Sphinx distutils command class
    """
    from sphinx.setup_command import BuildDoc

    return BuildDoc


def get_cpu_count():
    try:
        return len(os.sched_getaffinity(0))  # linux only
    except:
        pass

    try:
        return os.cpu_count()  # python 3.4+
    except:
        return 1  # default


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext, object):
    user_options = build_ext.user_options + [
        ('target=', None, "specify the CMake target to build")
    ]

    def initialize_options(self):
        self.target = "sonata_python"
        super(CMakeBuild, self).initialize_options()

    def run(self):
        try:
            out = subprocess.check_output(["cmake", "--version"])
        except OSError:
            raise RuntimeError(
                "CMake must be installed to build the following extensions: "
                + ", ".join(e.name for e in self.extensions)
            )

        if platform.system() == "Windows":
            cmake_version = LooseVersion(
                re.search(r"version\s*([\d.]+)", out.decode()).group(1)
            )
            if cmake_version < "3.1.0":
                raise RuntimeError("CMake >= 3.1.0 is required on Windows")

        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        cmake_args = [
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + extdir,
            "-DSONATA_TESTS={}".format(os.environ.get("SONATA_TESTS", "OFF")),
            "-DEXTLIB_FROM_SUBMODULES=ON",
            "-DSONATA_PYTHON=ON",
            "-DSONATA_VERSION=" + self.distribution.get_version(),
            "-DCMAKE_BUILD_TYPE=",
            "-DSONATA_CXX_WARNINGS=OFF",
            '-DPYTHON_EXECUTABLE=' + sys.executable
        ]

        optimize = "OFF" if self.debug else "ON"
        build_args = ["--config", optimize, "--target", self.target]

        if platform.system() == "Windows":
            cmake_args += [
                "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{}={}".format(
                    optimize.upper(), extdir
                )
            ]
            if sys.maxsize > 2 ** 32:
                cmake_args += ["-A", "x64"]
            build_args += ["--", "/m"]
        else:
            build_args += ["--", "-j{}".format(max(MIN_CPU_CORES, get_cpu_count()))]

        env = os.environ.copy()
        env["CXXFLAGS"] = '{} -DVERSION_INFO=\\"{}\\"'.format(
            env.get("CXXFLAGS", ""), self.distribution.get_version()
        )
        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)
        subprocess.check_call(
            ["cmake", ext.sourcedir] + cmake_args, cwd=self.build_temp, env=env
        )
        subprocess.check_call(
            ["cmake", "--build", "."] + build_args, cwd=self.build_temp
        )


class PkgTest(test):
    """Custom disutils command that acts like as a replacement
    for the "test" command.
    """

    new_commands = [('test_ext', lambda self: True), ('test_doc', lambda self: True)]
    sub_commands = test.sub_commands + new_commands

    def run(self):
        super(PkgTest, self).run()
        self.run_command('test_ext')
        self.run_command('test_doc')


install_requires = [
    REQUIRED_NUMPY_VERSION,
]

setup_requires = [
    "setuptools_scm",
]

doc_requires = [
    "sphinx-bluebrain-theme"
]

# HACK: `setup_requires` uses `easy_install` to do the 'pre-installation' of
# numpy, it doesn't realize it has to install numpy version that supports py2.
if sys.version_info[0] == 3:
    setup_requires.append(REQUIRED_NUMPY_VERSION)

with open('README.rst') as f:
    README = f.read()

setup(
    name="libsonata",
    description='SONATA files reader',
    author="Blue Brain Project, EPFL",
    long_description=README,
    long_description_content_type='text/x-rst',
    license="LGPLv3",
    url='https://github.com/BlueBrain/libsonata',
    classifiers=[
        "License :: OSI Approved :: GNU Lesser General Public License v3 (LGPLv3)",
    ],
    ext_modules=[CMakeExtension("libsonata._libsonata")],
    cmdclass=lazy_dict(
        build_ext=CMakeBuild,
        test_ext=CMakeBuild,
        test=PkgTest,
        test_doc=get_sphinx_command,
    ),
    zip_safe=False,
    setup_requires=setup_requires + doc_requires if "test" in sys.argv or "test_doc" in sys.argv
    else setup_requires,
    install_requires=install_requires,
    extras_require={
        'docs': ['sphinx-bluebrain-theme'],
    },
    use_scm_version={"local_scheme": "no-local-version",
                     },
    package_dir={"": "python"},
    packages=['libsonata',
              ],
    test_suite='tests.test_suite'
)
