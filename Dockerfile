// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

FROM ubuntu:14.04
RUN apt-get update
RUN apt-get install -y build-essential
RUN apt-get install -yq automake git cmake ninja-build ccache libncurses5-dev liblzma-dev libreadline-dev libgmp3-dev libmpfr-dev autoconf libtool python-dev texlive-extra-utils clang libssl-dev libsqlite3-dev pkg-config libbz2-dev
RUN apt-get install git
RUN git clone https://github.com/dropbox/pyston.git ~/pyston
RUN mkdir ~/pyston_deps
RUN git clone https://github.com/llvm-mirror/llvm.git ~/pyston_deps/llvm-trunk
RUN git clone https://github.com/llvm-mirror/clang.git ~/pyston_deps/llvm-trunk/tools/clang
RUN cd ~/pyston && git config --global user.email "docker@pyston.com" && git config --global user.name "docker"
RUN cd ~/pyston && git submodule update --init --recursive build_deps &&make llvm_up && make pyston_release
