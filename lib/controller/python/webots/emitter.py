# Copyright 1996-2021 Cyberbotics Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import ctypes
import sys
import typing
from webots.wb import wb

wb.wb_emitter_send.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]


class Emitter:
    def __init__(self, name: str):
        self._ref = wb.wb_robot_get_device(str.encode(name))

    def send(self, message: typing.Union[str, bytes], length: int = None):
        if isinstance(message, str):
            wb.wb_emitter_send(self._ref, str.encode(message), len(message) + 1)
        elif isinstance(message, bytes):
            if length is None:
                print('Emitter.send(): missing byte buffer length', file=sys.stderr)
            else:
                wb.wb_emitter_send(self._ref, message, length)
        else:
            print('Emitter.send(): unsupported data type', file=sys.stderr)
