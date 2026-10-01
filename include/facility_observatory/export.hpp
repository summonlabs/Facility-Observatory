// Facility Observatory - shared library export decoration.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_EXPORT_HPP
#define FACILITY_OBSERVATORY_EXPORT_HPP

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(FO_SHARED_BUILD)
#if defined(FO_EXPORTS)
#define FO_API __declspec(dllexport)
#else
#define FO_API __declspec(dllimport)
#endif
#else
#define FO_API
#endif
#elif defined(FO_SHARED_BUILD) && (defined(__GNUC__) || defined(__clang__))
#define FO_API __attribute__((visibility("default")))
#else
#define FO_API
#endif

// Marks declarations that are part of the frozen 1.x surface.
#define FO_PUBLIC_API FO_API

#endif  // FACILITY_OBSERVATORY_EXPORT_HPP
