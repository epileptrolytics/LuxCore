#include <string>
namespace luxrays { namespace ocl {
std::string KernelSource_trianglemesh_funcs = 
"#line 2 \"trianglemesh_funcs.cl\"\n"
"\n"
"/***************************************************************************\n"
" *   Copyright (C) 1998-2010 by authors (see AUTHORS.txt )                 *\n"
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
"float3 Mesh_GetGeometryNormal(__global Point *vertices,\n"
"		__global Triangle *triangles, const uint triIndex) {\n"
"	__global Triangle *tri = &triangles[triIndex];\n"
"	const float3 p0 = vload3(0, &vertices[tri->v[0]].x);\n"
"	const float3 p1 = vload3(0, &vertices[tri->v[1]].x);\n"
"	const float3 p2 = vload3(0, &vertices[tri->v[2]].x);\n"
"\n"
"	return Triangle_GetGeometryNormal(p0, p1, p2);\n"
"}\n"
"\n"
"float3 Mesh_InterpolateNormal(__global Vector *normals, __global Triangle *triangles,\n"
"		const uint triIndex, const float b1, const float b2) {\n"
"	__global Triangle *tri = &triangles[triIndex];\n"
"	const float3 n0 = vload3(0, &normals[tri->v[0]].x);\n"
"	const float3 n1 = vload3(0, &normals[tri->v[1]].x);\n"
"	const float3 n2 = vload3(0, &normals[tri->v[2]].x);\n"
"\n"
"	const float b0 = 1.f - b1 - b2;\n"
"	return Triangle_InterpolateNormal(n0, n1, n2, b0, b1, b2);\n"
"}\n"
"\n"
"float2 Mesh_InterpolateUV(__global UV *vertUVs, __global Triangle *triangles,\n"
"		const uint triIndex, const float b1, const float b2) {\n"
"	__global Triangle *tri = &triangles[triIndex];\n"
"	const float2 uv0 = vload2(0, &vertUVs[tri->v[0]].u);\n"
"	const float2 uv1 = vload2(0, &vertUVs[tri->v[1]].u);\n"
"	const float2 uv2 = vload2(0, &vertUVs[tri->v[2]].u);\n"
"\n"
"	const float b0 = 1.f - b1 - b2;\n"
"	return Triangle_InterpolateUV(uv0, uv1, uv2, b0, b1, b2);\n"
"}\n"
; } }