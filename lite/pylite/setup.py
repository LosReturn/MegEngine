# -*- coding: utf-8 -*-
# MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
#
# Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

import os
import re
import pathlib
import platform
from distutils.file_util import copy_file
from setuptools import setup, find_packages, Extension
from setuptools.command.build_ext import build_ext as _build_ext

class PrecompiledExtesion(Extension):
    def __init__(self, name):
        super().__init__(name, sources=[])

class build_ext(_build_ext):

    def build_extension(self, ext):
        if not isinstance(ext, PrecompiledExtesion):
            return super().build_extension(ext)

        if not self.inplace:
            fullpath = self.get_ext_fullpath(ext.name)
            extdir = pathlib.Path(fullpath)
            extdir.parent.mkdir(parents=True, exist_ok=True)

            modpath = self.get_ext_fullname(ext.name).split('.')
            if platform.system() == 'Windows':
                modpath[-1] += '.dll'
            elif platform.system() == 'Darwin':
                modpath[-1] += '.dylib'
            else:
                modpath[-1] += '.so'
            modpath = str(pathlib.Path(*modpath).resolve())

            copy_file(modpath, fullpath, verbose=self.verbose, dry_run=self.dry_run)

v = {}
with open("megenginelite/version.py") as fp:
    exec(fp.read(), v)
__version__ = v['__version__']

email = 'megengine@megvii.com'
# https://www.python.org/dev/peps/pep-0440
# Public version identifiers: [N!]N(.N)*[{a|b|rc}N][.postN][.devN]
# Local version identifiers: <public version identifier>[+<local version label>]
# PUBLIC_VERSION_POSTFIX use to handle rc or dev info
public_version_postfix = os.environ.get('PUBLIC_VERSION_POSTFIX')
if public_version_postfix:
    __version__ = '{}{}'.format(__version__, public_version_postfix)

local_version = []
strip_sdk_info = os.environ.get('STRIP_SDK_INFO', 'False').lower()
sdk_name = os.environ.get('SDK_NAME', 'cpu')
if 'true' == strip_sdk_info:
    print('wheel version strip sdk info')
else:
    local_version.append(sdk_name)
local_postfix = os.environ.get('LOCAL_VERSION')
if local_postfix:
    local_version.append(local_postfix)
if len(local_version):
    __version__ = '{}+{}'.format(__version__, '.'.join(local_version))

packages = find_packages()
megenginelite_data = [
    str(f.relative_to('megenginelite'))
    for f in pathlib.Path('megenginelite').glob('**/*')
]

if platform.system() == 'Windows':
    megenginelite_data.remove('libs\\liblite_shared.dll')
elif platform.system() == 'Darwin':
    megenginelite_data.remove('libs/liblite_shared.dylib')
else:
    megenginelite_data.remove('libs/liblite_shared.so')

with open('requires.txt') as f:
    requires = f.read().splitlines()

prebuild_modules=[PrecompiledExtesion('megenginelite.libs.liblite_shared')]
setup_kwargs = dict(
    name=package_name,
    version=__version__,
    description='Inference Framework for MegEngine',
    author='Megvii Engine Team',
    author_email=email,
    packages=packages,
    package_data={
        'megenginelite': megenginelite_data,
    },
    ext_modules=prebuild_modules,
    install_requires=requires,
    cmdclass={'build_ext': build_ext},
)
setup_kwargs.update(dict(
    classifiers=[
    'Development Status :: 3 - Alpha',
    'Intended Audience :: Developers',
    'Intended Audience :: Education',
    'Intended Audience :: Science/Research',
    'License :: OSI Approved :: Apache Software License',
    'Programming Language :: C++',
    'Programming Language :: Python :: 3',
    'Programming Language :: Python :: 3.5',
    'Programming Language :: Python :: 3.6',
    'Programming Language :: Python :: 3.7',
    'Programming Language :: Python :: 3.8',
    'Topic :: Scientific/Engineering',
    'Topic :: Scientific/Engineering :: Mathematics',
    'Topic :: Scientific/Engineering :: Artificial Intelligence',
    'Topic :: Software Development',
    'Topic :: Software Development :: Libraries',
    'Topic :: Software Development :: Libraries :: Python Modules',
    ],
    license='Apache 2.0',
    keywords='megengine deep learning',
    data_files = [("megengine", [
        "../LICENSE",
        "../ACKNOWLEDGMENTS",
    ])]
))

setup(**setup_kwargs)