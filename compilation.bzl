# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Rules for compiling IREE executables, modules, and archives."""

load("//build_tools/embed_data:build_defs.bzl", "cc_embed_data")

# TODO(benvanik): port to a full starlark rule, document, etc.
def iree_bytecode_module(
        name,
        src,
        translation = "-iree-mlir-to-vm-bytecode-module",
        translate_tool = "//iree/tools:iree-translate",
        cc_namespace = None,
        visibility = None):
    native.genrule(
        name = name,
        srcs = [src],
        outs = [
            "%s.module" % (name),
        ],
        cmd = " && ".join([
            " ".join([
                "$(location %s)" % (translate_tool),
                translation,
                "-o $(location %s.module)" % (name),
                "$(location %s)" % (src),
            ]),
        ]),
        tools = [translate_tool],
        message = "Compiling IREE module %s..." % (name),
        output_to_bindir = 1,
    )

    # Embed the module for use in C++. This avoids the need for file IO in
    # tests and samples that would otherwise complicate execution/porting.
    if cc_namespace:
        cc_embed_data(
            name = "%s_cc" % (name),
            identifier = name,
            srcs = ["%s.module" % (name)],
            cc_file_output = "%s.cc" % (name),
            h_file_output = "%s.h" % (name),
            cpp_namespace = cc_namespace,
            visibility = visibility,
            flatten = True,
        )
