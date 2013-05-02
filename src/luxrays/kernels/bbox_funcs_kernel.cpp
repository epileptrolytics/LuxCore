#include <string>
namespace luxrays { namespace ocl {
std::string KernelSource_bbox_funcs = 
"#line 2 \"bbox_funcs.cl\"\n"
"\n"
"/***************************************************************************\n"
" *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *\n"
" *                                                                         *\n"
" *   This file is part of LuxRays.                                         *\n"
" *                                                                         *\n"
" *   LuxRays is free software; you can redistribute it and/or modify       *\n"
" *   it under the terms of the GNU General Public License as published by  *\n"
" *   the Free Software Foundation; either version 3 of the License, or     *\n"
" *   (at your option) any later version.                                   *\n"
" *                                                                         *\n"
" *   LuxRays is distributed in the hope that it will be useful,            *\n"
" *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *\n"
" *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *\n"
" *   GNU General Public License for more details.                          *\n"
" *                                                                         *\n"
" *   You should have received a copy of the GNU General Public License     *\n"
" *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *\n"
" *                                                                         *\n"
" *   LuxRays website: http://www.luxrender.net                             *\n"
" ***************************************************************************/\n"
"\n"
"int BBox_IntersectP(const float3 pMin, const float3 pMax,\n"
"		const float3 rayOrig, const float3 invRayDir,\n"
"		const float mint, const float maxt) {\n"
"	const float3 l1 = (pMin - rayOrig) * invRayDir;\n"
"	const float3 l2 = (pMax - rayOrig) * invRayDir;\n"
"	const float3 tNear = fmin(l1, l2);\n"
"	const float3 tFar = fmax(l1, l2);\n"
"\n"
"	float t0 = fmax(fmax(fmax(tNear.x, tNear.y), fmax(tNear.x, tNear.z)), mint);\n"
"    float t1 = fmin(fmin(fmin(tFar.x, tFar.y), fmin(tFar.x, tFar.z)), maxt);\n"
"\n"
"	return (t1 > t0);\n"
"}\n"
; } }