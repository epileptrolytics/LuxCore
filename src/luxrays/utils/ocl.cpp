/***************************************************************************
 *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *
 *                                                                         *
 *   This file is part of LuxRays.                                         *
 *                                                                         *
 *   LuxRays is free software; you can redistribute it and/or modify       *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxRays is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 *                                                                         *
 *   LuxRays website: http://www.luxrender.net                             *
 ***************************************************************************/

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <iostream>
#include <fstream>
#include <string.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

#include "luxrays/luxrays.h"
#include "luxrays/core/utils.h"
#include "luxrays/utils/ocl.h"

using namespace luxrays;

// Helper function to get error string
std::string luxrays::oclErrorString(cl_int error) {
	switch (error) {
		case CL_SUCCESS:
			return "CL_SUCCESS";
		case CL_DEVICE_NOT_FOUND:
			return "CL_DEVICE_NOT_FOUND";
		case CL_DEVICE_NOT_AVAILABLE:
			return "CL_DEVICE_NOT_AVAILABLE";
		case CL_COMPILER_NOT_AVAILABLE:
			return "CL_COMPILER_NOT_AVAILABLE";
		case CL_MEM_OBJECT_ALLOCATION_FAILURE:
			return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
		case CL_OUT_OF_RESOURCES:
			return "CL_OUT_OF_RESOURCES";
		case CL_OUT_OF_HOST_MEMORY:
			return "CL_OUT_OF_HOST_MEMORY";
		case CL_PROFILING_INFO_NOT_AVAILABLE:
			return "CL_PROFILING_INFO_NOT_AVAILABLE";
		case CL_MEM_COPY_OVERLAP:
			return "CL_MEM_COPY_OVERLAP";
		case CL_IMAGE_FORMAT_MISMATCH:
			return "CL_IMAGE_FORMAT_MISMATCH";
		case CL_IMAGE_FORMAT_NOT_SUPPORTED:
			return "CL_IMAGE_FORMAT_NOT_SUPPORTED";
		case CL_BUILD_PROGRAM_FAILURE:
			return "CL_BUILD_PROGRAM_FAILURE";
		case CL_MAP_FAILURE:
			return "CL_MAP_FAILURE";
#ifdef CL_VERSION_1_1
		case CL_MISALIGNED_SUB_BUFFER_OFFSET:
			return "CL_MISALIGNED_SUB_BUFFER_OFFSET";
		case CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST:
			return "CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST";
#endif
		case CL_INVALID_VALUE:
			return "CL_INVALID_VALUE";
		case CL_INVALID_DEVICE_TYPE:
			return "CL_INVALID_DEVICE_TYPE";
		case CL_INVALID_PLATFORM:
			return "CL_INVALID_PLATFORM";
		case CL_INVALID_DEVICE:
			return "CL_INVALID_DEVICE";
		case CL_INVALID_CONTEXT:
			return "CL_INVALID_CONTEXT";
		case CL_INVALID_QUEUE_PROPERTIES:
			return "CL_INVALID_QUEUE_PROPERTIES";
		case CL_INVALID_COMMAND_QUEUE:
			return "CL_INVALID_COMMAND_QUEUE";
		case CL_INVALID_HOST_PTR:
			return "CL_INVALID_HOST_PTR";
		case CL_INVALID_MEM_OBJECT:
			return "CL_INVALID_MEM_OBJECT";
		case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR:
			return "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
		case CL_INVALID_IMAGE_SIZE:
			return "CL_INVALID_IMAGE_SIZE";
		case CL_INVALID_SAMPLER:
			return "CL_INVALID_SAMPLER";
		case CL_INVALID_BINARY:
			return "CL_INVALID_BINARY";
		case CL_INVALID_BUILD_OPTIONS:
			return "CL_INVALID_BUILD_OPTIONS";
		case CL_INVALID_PROGRAM:
			return "CL_INVALID_PROGRAM";
		case CL_INVALID_PROGRAM_EXECUTABLE:
			return "CL_INVALID_PROGRAM_EXECUTABLE";
		case CL_INVALID_KERNEL_NAME:
			return "CL_INVALID_KERNEL_NAME";
		case CL_INVALID_KERNEL_DEFINITION:
			return "CL_INVALID_KERNEL_DEFINITION";
		case CL_INVALID_KERNEL:
			return "CL_INVALID_KERNEL";
		case CL_INVALID_ARG_INDEX:
			return "CL_INVALID_ARG_INDEX";
		case CL_INVALID_ARG_VALUE:
			return "CL_INVALID_ARG_VALUE";
		case CL_INVALID_ARG_SIZE:
			return "CL_INVALID_ARG_SIZE";
		case CL_INVALID_KERNEL_ARGS:
			return "CL_INVALID_KERNEL_ARGS";
		case CL_INVALID_WORK_DIMENSION:
			return "CL_INVALID_WORK_DIMENSION";
		case CL_INVALID_WORK_GROUP_SIZE:
			return "CL_INVALID_WORK_GROUP_SIZE";
		case CL_INVALID_WORK_ITEM_SIZE:
			return "CL_INVALID_WORK_ITEM_SIZE";
		case CL_INVALID_GLOBAL_OFFSET:
			return "CL_INVALID_GLOBAL_OFFSET";
		case CL_INVALID_EVENT_WAIT_LIST:
			return "CL_INVALID_EVENT_WAIT_LIST";
		case CL_INVALID_EVENT:
			return "CL_INVALID_EVENT";
		case CL_INVALID_OPERATION:
			return "CL_INVALID_OPERATION";
		case CL_INVALID_GL_OBJECT:
			return "CL_INVALID_GL_OBJECT";
		case CL_INVALID_BUFFER_SIZE:
			return "CL_INVALID_BUFFER_SIZE";
		case CL_INVALID_MIP_LEVEL:
			return "CL_INVALID_MIP_LEVEL";
		case CL_INVALID_GLOBAL_WORK_SIZE:
			return "CL_INVALID_GLOBAL_WORK_SIZE";
		default:
			return boost::lexical_cast<std::string > (error);
	}
}

//------------------------------------------------------------------------------
// oclKernelCache
//------------------------------------------------------------------------------

cl::Program *oclKernelCache::ForcedCompile(cl::Context &context, cl::Device &device,
		const std::string &kernelsParameters, const std::string &kernelSource,
		cl::STRING_CLASS *error) {
	cl::Program *program = NULL;

	try {
		cl::Program::Sources source(1, std::make_pair(kernelSource.c_str(), kernelSource.length()));
		program = new cl::Program(context, source);

		VECTOR_CLASS<cl::Device> buildDevice;
		buildDevice.push_back(device);
		program->build(buildDevice, kernelsParameters.c_str());
	} catch (cl::Error err) {
		const std::string clerr = program->getBuildInfo<CL_PROGRAM_BUILD_LOG>(device);

		std::stringstream ss;
		ss << "ERROR " << err.what() << "[" << luxrays::oclErrorString(err.err()) << "]:" <<
				std::endl << clerr << std::endl;
		*error = ss.str();

		if (program)
			delete program;
		program = NULL;
	}

	return program;
}

//------------------------------------------------------------------------------
// oclKernelVolatileCache
//------------------------------------------------------------------------------

oclKernelVolatileCache::oclKernelVolatileCache() {

}

oclKernelVolatileCache::~oclKernelVolatileCache() {
	for (std::vector<char *>::iterator it = kernels.begin(); it != kernels.end(); it++)
		delete[] (*it);
}

cl::Program *oclKernelVolatileCache::Compile(cl::Context &context, cl::Device& device,
		const std::string &kernelsParameters, const std::string &kernelSource,
		bool *cached, cl::STRING_CLASS *error) {
	// Check if the kernel is available in the cache
	std::map<std::string, cl::Program::Binaries>::iterator it = kernelCache.find(kernelsParameters);

	if (it == kernelCache.end()) {
		// It isn't available, compile the source
		cl::Program *program = ForcedCompile(
				context, device, kernelsParameters, kernelSource, error);
		if (!program)
			return NULL;

		// Obtain the binaries of the sources
		VECTOR_CLASS<char *> bins = program->getInfo<CL_PROGRAM_BINARIES>();
		assert (bins.size() == 1);
		VECTOR_CLASS<size_t> sizes = program->getInfo<CL_PROGRAM_BINARY_SIZES>();
		assert (sizes.size() == 1);

		if (sizes[0] > 0) {
			// Add the kernel to the cache
			char *bin = new char[sizes[0]];
			memcpy(bin, bins[0], sizes[0]);
			kernels.push_back(bin);

			kernelCache[kernelsParameters] = cl::Program::Binaries(1, std::make_pair(bin, sizes[0]));
		}

		if (cached)
			*cached = false;

		return program;
	} else {
		// Compile from the binaries
		VECTOR_CLASS<cl::Device> buildDevice;
		buildDevice.push_back(device);
		cl::Program *program = new cl::Program(context, buildDevice, it->second);
		program->build(buildDevice);

		if (cached)
			*cached = true;

		return program;
	}
}

//------------------------------------------------------------------------------
// oclKernelPersistentCache
//------------------------------------------------------------------------------

oclKernelPersistentCache::oclKernelPersistentCache(const std::string &applicationName) {
	appName = applicationName;

	// Just to be safe
	boost::replace_all(appName, ":", "-");
	boost::replace_all(appName, " ", "-");
	boost::replace_all(appName, "/", "-");
	boost::replace_all(appName, "\\", "-");

	// Crate the cache directory
	boost::filesystem::create_directories(boost::filesystem::temp_directory_path() / "kernel_cache" / appName);
}

oclKernelPersistentCache::~oclKernelPersistentCache() {
}

// Bob Jenkins's One-at-a-Time hash
// From: http://eternallyconfuzzled.com/tuts/algorithms/jsw_tut_hashing.aspx

std::string oclKernelPersistentCache::HashString(const std::string &ss) {
	const u_int hash = HashBin(ss.c_str(), ss.length());

	char buf[9];
	sprintf(buf, "%08x", hash);

	return std::string(buf);
}

u_int oclKernelPersistentCache::HashBin(const char *s, const size_t size) {
	u_int hash = 0;

	for (u_int i = 0; i < size; ++i) {
		hash += *s++;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

cl::Program *oclKernelPersistentCache::Compile(cl::Context &context, cl::Device& device,
		const std::string &kernelsParameters, const std::string &kernelSource,
		bool *cached, cl::STRING_CLASS *error) {
	// Check if the kernel is available in the cache

	cl::Platform platform = device.getInfo<CL_DEVICE_PLATFORM>();
	std::string platformName = platform.getInfo<CL_PLATFORM_VENDOR>();
	std::string deviceName = device.getInfo<CL_DEVICE_NAME>();
	std::string deviceUnits = ToString(device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>());
	std::string kernelName = HashString(kernelsParameters) + "-" + HashString(kernelSource) + ".ocl";
	boost::filesystem::path dirPath = boost::filesystem::temp_directory_path() / "kernel_cache" /
			appName / platformName / deviceName / deviceUnits;
	boost::filesystem::path filePath = dirPath / kernelName;
	const std::string fileName = filePath.generic_string();
	
	if (!boost::filesystem::exists(filePath)) {
		// It isn't available, compile the source
		cl::Program *program = ForcedCompile(
				context, device, kernelsParameters, kernelSource, error);
		if (!program)
			return NULL;

		// Obtain the binaries of the sources
		VECTOR_CLASS<char *> bins = program->getInfo<CL_PROGRAM_BINARIES>();
		assert (bins.size() == 1);
		VECTOR_CLASS<size_t> sizes = program->getInfo<CL_PROGRAM_BINARY_SIZES >();
		assert (sizes.size() == 1);

		// Create the file only if the binaries include something
		if (sizes[0] > 0) {
			// Add the kernel to the cache
			boost::filesystem::create_directories(dirPath);
			BOOST_OFSTREAM file(fileName.c_str(), std::ios_base::out | std::ios_base::binary);

			// Write the binary hash
			const u_int hashBin = HashBin(bins[0], sizes[0]);
			file.write((char *)&hashBin, sizeof(int));

			file.write(bins[0], sizes[0]);
			// Check for errors
			char buf[512];
			if (file.fail()) {
				sprintf(buf, "Unable to write kernel file cache %s", fileName.c_str());
				throw std::runtime_error(buf);
			}

			file.close();
		}

		if (cached)
			*cached = false;

		return program;
	} else {
		const size_t fileSize = boost::filesystem::file_size(filePath);

		if (fileSize > 4) {
			const size_t kernelSize = fileSize - 4;

			char *kernelBin = new char[kernelSize];

			BOOST_IFSTREAM file(fileName.c_str(), std::ios_base::in | std::ios_base::binary);

			// Read the binary hash
			u_int hashBin;
			file.read((char *)&hashBin, sizeof(int));

			file.read(kernelBin, kernelSize);
			// Check for errors
			char buf[512];
			if (file.fail()) {
				sprintf(buf, "Unable to read kernel file cache %s", fileName.c_str());
				throw std::runtime_error(buf);
			}

			file.close();

			// Check the binary hash
			if (hashBin != HashBin(kernelBin, kernelSize)) {
				// Something wrong in the file, remove the file and retry
				boost::filesystem::remove(filePath);
				return Compile(context, device, kernelsParameters, kernelSource, cached, error);
			} else {
				// Compile from the binaries
				VECTOR_CLASS<cl::Device> buildDevice;
				buildDevice.push_back(device);
				cl::Program *program = new cl::Program(context, buildDevice,
						cl::Program::Binaries(1, std::make_pair(kernelBin, kernelSize)));
				program->build(buildDevice);

				if (cached)
					*cached = true;

				delete[] kernelBin;

				return program;
			}
		} else {
			// Something wrong in the file, remove the file and retry
			boost::filesystem::remove(filePath);
			return Compile(context, device, kernelsParameters, kernelSource, cached, error);
		}
	}
}

#endif
