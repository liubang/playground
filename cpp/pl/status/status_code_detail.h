// Copyright (c) 2025 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#ifndef RAW_STATUS
#define RAW_STATUS(...)
#endif

#ifndef STATUS
#define STATUS(...)
#endif

#define COMMON_STATUS(...) RAW_STATUS(__VA_ARGS__)

COMMON_STATUS(OK, 0)
COMMON_STATUS(NotImplemented, 1)
COMMON_STATUS(DataCorruption, 2)
COMMON_STATUS(InvalidArg, 3)
COMMON_STATUS(InvalidConfig, 4)
COMMON_STATUS(QueueEmpty, 5)
COMMON_STATUS(QueueFull, 6)
COMMON_STATUS(QueueConflict, 7)
COMMON_STATUS(QueueInvalidItem, 8)
COMMON_STATUS(MonitorInitFailed, 12)
COMMON_STATUS(MonitorQueryFailed, 13)
COMMON_STATUS(MonitorWriteFailed, 14)
COMMON_STATUS(ConfigInvalidType, 15)
COMMON_STATUS(ConfigInvalidValue, 16)
COMMON_STATUS(ConfigUpdateFailed, 17)
COMMON_STATUS(ConfigValidateFailed, 18)
COMMON_STATUS(ConfigRedundantKey, 19)
COMMON_STATUS(ConfigKeyNotFound, 20)
COMMON_STATUS(AuthenticationFail, 25)
COMMON_STATUS(NotEnoughMemory, 26)
COMMON_STATUS(Interrupted, 27)
COMMON_STATUS(InvalidFormat, 33)
COMMON_STATUS(ReadOnlyMode, 34)
COMMON_STATUS(SerdeInsufficientLength, 40)
COMMON_STATUS(SerdeMissingFieldsAtEnd, 41)
COMMON_STATUS(SerdeVariantIndexExceeded, 42)
COMMON_STATUS(SerdeUnknownEnumValue, 43)
COMMON_STATUS(SerdeNotContainer, 44)
COMMON_STATUS(SerdeNotString, 45)
COMMON_STATUS(SerdeNotNumber, 46)
COMMON_STATUS(SerdeNotInteger, 47)
COMMON_STATUS(SerdeNotTable, 48)
COMMON_STATUS(SerdeKeyNotFound, 49)
COMMON_STATUS(SerdeInvalidJson, 50)
COMMON_STATUS(SerdeInvalidToml, 51)
COMMON_STATUS(NoApplication, 52)
COMMON_STATUS(CannotPushConfig, 53)
COMMON_STATUS(KVStoreNotFound, 60)
COMMON_STATUS(KVStoreGetError, 61)
COMMON_STATUS(KVStoreSetError, 62)
COMMON_STATUS(KVStoreOpenFailed, 63)
COMMON_STATUS(TokenMismatch, 64)
COMMON_STATUS(TokenDuplicated, 65)
COMMON_STATUS(TokenStale, 66)
COMMON_STATUS(TooManyTokens, 67)
COMMON_STATUS(KVStoreIterateError, 68)
COMMON_STATUS(IOError, 69)
COMMON_STATUS(FaultInjection, 70)
COMMON_STATUS(ConfigParseError, 71)
COMMON_STATUS(OSError, 72)

COMMON_STATUS(SSTableBuilderClosed, 100)

COMMON_STATUS(FoundBug, 998)
COMMON_STATUS(Unknown, 999)

#undef RAW_STATUS
#undef STATUS
