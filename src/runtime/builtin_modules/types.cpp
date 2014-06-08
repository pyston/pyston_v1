// Copyright (c) 2014 Dropbox, Inc.
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

#include "codegen/compvars.h"
#include "core/types.h"
#include "runtime/gc_runtime.h"
#include "runtime/types.h"

namespace pyston {

BoxedModule* types_module;


void setupTypes() {
    types_module = createModule("types", "__builtin__");

    types_module->giveAttr("NoneType", none_cls);
    types_module->giveAttr("TypeType", type_cls);

    types_module->giveAttr("BooleanType", bool_cls);
    types_module->giveAttr("IntType", int_cls);
    // types.LongType
    types_module->giveAttr("FloatType", float_cls);
    // types.ComplexType
    types_module->giveAttr("StringType", str_cls);
    // types.UnicodeType
    types_module->giveAttr("TupleType", tuple_cls);
    types_module->giveAttr("ListType", list_cls);
    types_module->giveAttr("DictType", dict_cls);
    types_module->giveAttr("DictionaryType", dict_cls);
    types_module->giveAttr("FunctionType", function_cls);
    // types.LambdaType
    // types.GeneratorType
    // types.CodeType
    // types.ClassType
    // types.InstanceType
    types_module->giveAttr("MethodType", instancemethod_cls);
    types_module->giveAttr("UnboundMethodType", instancemethod_cls);
    // types.BuiltinFunctionType, types.BuiltinMethodType
    types_module->giveAttr("ModuleType", module_cls);
    types_module->giveAttr("FileType", file_cls);
    // types.XRangeType
    types_module->giveAttr("SliceType", slice_cls);
    // types.EllipsisType
    // types.TracebackType
    // types.FrameType
    // types.BufferType
    // types.DictProxyType
    // types.NotImplementedType
    // types.GetSetDescriptorType
    types_module->giveAttr("MemberDescriptorType", member_cls);
    // types.StringTypes
}
}
