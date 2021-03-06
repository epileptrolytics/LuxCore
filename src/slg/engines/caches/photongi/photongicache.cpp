/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <boost/format.hpp>

#if defined(_OPENMP)
#include <omp.h>
#endif

#include "slg/samplers/sobol.h"
#include "slg/utils/pathdepthinfo.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/caches/photongi/tracephotonsthread.h"
#include "slg/engines/caches/photongi/tracevisibilitythread.h"

using namespace std;
using namespace luxrays;
using namespace slg;

// TODO: serialization

//------------------------------------------------------------------------------
// PhotonGICache
//------------------------------------------------------------------------------

PhotonGICache::PhotonGICache(const Scene *scn, const PhotonGICacheParams &p) :
		scene(scn), params(p),
		samplerSharedData(nullptr),
		directPhotonTracedCount(0),
		indirectPhotonTracedCount(0),
		causticPhotonTracedCount(0),
		visibilitySobolSharedData(131, nullptr),
		visibilityParticlesOctree(nullptr),
		directPhotonsBVH(nullptr),
		indirectPhotonsBVH(nullptr),
		causticPhotonsBVH(nullptr),
		radiancePhotonsBVH(nullptr) {
	if (!params.direct.enabled) {
		if (params.indirect.enabled) {
			// I need to initialize direct cache parameters to be able to compute radiance cache
			params.direct.maxSize = params.indirect.maxSize / params.photon.maxPathDepth;

			params.direct.lookUpMaxCount = params.indirect.lookUpMaxCount;
			params.direct.lookUpRadius = params.indirect.lookUpRadius;
			params.direct.lookUpNormalAngle = params.indirect.lookUpNormalAngle;
		} else
			params.direct.maxSize = 0;
	}

	if (!params.indirect.enabled)
		params.indirect.maxSize = 0;

	if (!params.caustic.enabled)
		params.caustic.maxSize = 0;

	params.visibility.lookUpRadius2 = params.visibility.lookUpRadius * params.visibility.lookUpRadius;
	params.direct.lookUpRadius2 = params.direct.lookUpRadius * params.direct.lookUpRadius;
	params.indirect.lookUpRadius2 = params.indirect.lookUpRadius * params.indirect.lookUpRadius;
	params.caustic.lookUpRadius2 = params.caustic.lookUpRadius * params.caustic.lookUpRadius;
}

PhotonGICache::~PhotonGICache() {
	delete samplerSharedData;
	
	delete visibilityParticlesOctree;

	delete directPhotonsBVH;
	delete indirectPhotonsBVH;
	delete causticPhotonsBVH;
	delete radiancePhotonsBVH;
}

void PhotonGICache::TraceVisibilityParticles() {
	const size_t renderThreadCount = boost::thread::hardware_concurrency();
	vector<TraceVisibilityThread *> renderThreads(renderThreadCount, nullptr);
	SLG_LOG("PhotonGI trace visibility particles thread count: " << renderThreads.size());

	// Initialize the Octree where to store the visibility points
	visibilityParticlesOctree = new PGCIOctree(visibilityParticles, scene->dataSet->GetBBox(),
			params.visibility.lookUpRadius, params.visibility.lookUpNormalAngle);

	globalVisibilityParticlesCount = 0;
	visibilityCacheLookUp = 0;
	visibilityCacheHits = 0;
	visibilityWarmUp = true;

	// Create the visibility particles tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i)
		renderThreads[i] = new TraceVisibilityThread(*this, i);

	// Start visibility particles tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i)
		renderThreads[i]->Start();
	
	// Wait for the end of visibility particles tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i) {
		renderThreads[i]->Join();

		delete renderThreads[i];
	}

	visibilityParticles.shrink_to_fit();
	SLG_LOG("PhotonGI visibility total entries: " << visibilityParticles.size());
}

void PhotonGICache::TracePhotons(vector<Photon> &directPhotons, vector<Photon> &indirectPhotons,
		vector<Photon> &causticPhotons) {
	const size_t renderThreadCount = boost::thread::hardware_concurrency();
	vector<TracePhotonsThread *> renderThreads(renderThreadCount, nullptr);
	SLG_LOG("PhotonGI trace photons thread count: " << renderThreads.size());
	
	globalPhotonsCounter = 0;
	globalDirectPhotonsTraced = 0;
	globalIndirectPhotonsTraced = 0;
	globalCausticPhotonsTraced = 0;
	globalDirectSize = 0;
	globalIndirectSize = 0;
	globalCausticSize = 0;

	// Create the photon tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i)
		renderThreads[i] = new TracePhotonsThread(*this, i);

	// Start photon tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i)
		renderThreads[i]->Start();
	
	// Wait for the end of photon tracing threads
	for (size_t i = 0; i < renderThreads.size(); ++i) {
		renderThreads[i]->Join();

		// Copy all photons
		directPhotons.insert(directPhotons.end(), renderThreads[i]->directPhotons.begin(),
				renderThreads[i]->directPhotons.end());
		indirectPhotons.insert(indirectPhotons.end(), renderThreads[i]->indirectPhotons.begin(),
				renderThreads[i]->indirectPhotons.end());
		causticPhotons.insert(causticPhotons.end(), renderThreads[i]->causticPhotons.begin(),
				renderThreads[i]->causticPhotons.end());
		radiancePhotons.insert(radiancePhotons.end(), renderThreads[i]->radiancePhotons.begin(),
				renderThreads[i]->radiancePhotons.end());

		delete renderThreads[i];
	}

	directPhotonTracedCount = globalDirectPhotonsTraced;
	indirectPhotonTracedCount = globalIndirectPhotonsTraced;
	causticPhotonTracedCount = globalCausticPhotonsTraced;
	
	directPhotons.shrink_to_fit();
	indirectPhotons.shrink_to_fit();
	causticPhotons.shrink_to_fit();
	radiancePhotons.shrink_to_fit();

	// globalPhotonsCounter isn't exactly the number: there is an error due
	// last bucket of work likely being smaller than work bucket size
	SLG_LOG("PhotonGI total photon traced: " << globalPhotonsCounter);
	SLG_LOG("PhotonGI total direct photon stored: " << directPhotons.size() <<
			" (" << directPhotonTracedCount << " traced)");
	SLG_LOG("PhotonGI total indirect photon stored: " << indirectPhotons.size() <<
			" (" << indirectPhotonTracedCount << " traced)");
	SLG_LOG("PhotonGI total caustic photon stored: " << causticPhotons.size() <<
			" (" << causticPhotonTracedCount << " traced)");
	SLG_LOG("PhotonGI total radiance photon stored: " << radiancePhotons.size());
}

void PhotonGICache::AddOutgoingRadiance(RadiancePhoton &radiacePhoton, const PGICPhotonBvh *photonsBVH,
			const u_int photonTracedCount) const {
	if (photonsBVH) {
		vector<NearPhoton> entries;
		entries.reserve(photonsBVH->GetEntryMaxLookUpCount());

		float maxDistance2;
		photonsBVH->GetAllNearEntries(entries, radiacePhoton.p, radiacePhoton.n, maxDistance2);

		if (entries.size() > 0) {
			Spectrum result;
			for (auto const &nearPhoton : entries) {
				const Photon *photon = (const Photon *)nearPhoton.photon;

				// Using a box filter here (i.e. multiply by 1.0)
				result += photon->alpha * AbsDot(radiacePhoton.n, -photon->d);
			}

			result /= photonTracedCount * maxDistance2 * M_PI;

			radiacePhoton.outgoingRadiance += result;
		}
	}
}

void PhotonGICache::FillRadiancePhotonData(RadiancePhoton &radiacePhoton) {
	// This value was saved at RadiancePhoton creation time
	const Spectrum bsdfEvaluateTotal = radiacePhoton.outgoingRadiance;

	radiacePhoton.outgoingRadiance = Spectrum();
	AddOutgoingRadiance(radiacePhoton, directPhotonsBVH, directPhotonTracedCount);
	AddOutgoingRadiance(radiacePhoton, indirectPhotonsBVH, indirectPhotonTracedCount);
	AddOutgoingRadiance(radiacePhoton, causticPhotonsBVH, causticPhotonTracedCount);

	radiacePhoton.outgoingRadiance *= bsdfEvaluateTotal * INV_PI;
}

void PhotonGICache::FillRadiancePhotonsData() {
	double lastPrintTime = WallClockTime();
	atomic<u_int> counter(0);
	
	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < radiancePhotons.size(); ++i) {
		const int tid =
#if defined(_OPENMP)
			omp_get_thread_num()
#else
			0
#endif
			;

		if (tid == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				SLG_LOG("Radiance photon filled entries: " << counter << "/" << radiancePhotons.size() <<" (" << (u_int)((100.0 * counter) / radiancePhotons.size()) << "%)");
				lastPrintTime = now;
			}
		}
		
		FillRadiancePhotonData(radiancePhotons[i]);
		
		++counter;
	}
}

void PhotonGICache::Preprocess() {
	//--------------------------------------------------------------------------
	// Trace visibility particles
	//--------------------------------------------------------------------------

	// Visibility information are used only by Metropolis sampler
	if ((params.samplerType == PGIC_SAMPLER_METROPOLIS) && params.visibility.enabled)
		TraceVisibilityParticles();

	//--------------------------------------------------------------------------
	// Fill all photon vectors
	//--------------------------------------------------------------------------

	TracePhotons(directPhotons, indirectPhotons, causticPhotons);

	//--------------------------------------------------------------------------
	// Free visibility map
	//--------------------------------------------------------------------------

	if ((params.samplerType == PGIC_SAMPLER_METROPOLIS) && params.visibility.enabled) {
		delete visibilityParticlesOctree;
		visibilityParticlesOctree = nullptr;
		visibilityParticles.clear();
		visibilityParticles.shrink_to_fit();
	}
	
	//--------------------------------------------------------------------------
	// Direct light photon map
	//--------------------------------------------------------------------------

	if ((directPhotons.size() > 0) && (params.direct.enabled || params.indirect.enabled)) {
		SLG_LOG("PhotonGI building direct photons BVH");
		directPhotonsBVH = new PGICPhotonBvh(directPhotons, params.direct.lookUpMaxCount,
				params.direct.lookUpRadius, params.direct.lookUpNormalAngle);
	}

	//--------------------------------------------------------------------------
	// Indirect light photon map
	//--------------------------------------------------------------------------

	if ((indirectPhotons.size() > 0) && params.indirect.enabled) {
		SLG_LOG("PhotonGI building indirect photons BVH");
		indirectPhotonsBVH = new PGICPhotonBvh(indirectPhotons, params.indirect.lookUpMaxCount,
				params.indirect.lookUpRadius, params.indirect.lookUpNormalAngle);
	}

	//--------------------------------------------------------------------------
	// Caustic photon map
	//--------------------------------------------------------------------------

	if ((causticPhotons.size() > 0) && params.caustic.enabled) {
		SLG_LOG("PhotonGI building caustic photons BVH");
		causticPhotonsBVH = new PGICPhotonBvh(causticPhotons, params.caustic.lookUpMaxCount,
				params.caustic.lookUpRadius, params.caustic.lookUpNormalAngle);
	}

	//--------------------------------------------------------------------------
	// Radiance photon map
	//--------------------------------------------------------------------------

	if ((radiancePhotons.size() > 0) && params.indirect.enabled) {	
		SLG_LOG("PhotonGI building radiance photon data");
		FillRadiancePhotonsData();

		SLG_LOG("PhotonGI building radiance photons BVH");
		radiancePhotonsBVH = new PGICRadiancePhotonBvh(radiancePhotons, params.indirect.lookUpMaxCount,
				params.indirect.lookUpRadius, params.indirect.lookUpNormalAngle);
	}
	
	//--------------------------------------------------------------------------
	// Check what I can free because it is not going to be used during
	// the rendering
	//--------------------------------------------------------------------------
	
	if (!params.direct.enabled) {
		delete directPhotonsBVH;
		directPhotonsBVH = nullptr;
		directPhotons.clear();
		directPhotons.shrink_to_fit();
	}

	// I can always free indirect photon map because I'm going to use the
	// radiance map if the indirect cache is enabled
	delete indirectPhotonsBVH;
	indirectPhotonsBVH = nullptr;
	indirectPhotons.clear();
	indirectPhotons.shrink_to_fit();

	if (!params.caustic.enabled) {
		delete causticPhotonsBVH;
		causticPhotonsBVH = nullptr;
		causticPhotons.clear();
		causticPhotons.shrink_to_fit();
	}
	
	//--------------------------------------------------------------------------
	// Print some statistics about memory usage
	//--------------------------------------------------------------------------

	size_t totalMemUsage = 0;
	if (directPhotonsBVH) {
		SLG_LOG("PhotonGI direct cache photons memory usage: " << ToMemString(directPhotons.size() * sizeof(Photon)));
		SLG_LOG("PhotonGI direct cache BVH memory usage: " << ToMemString(directPhotonsBVH->GetMemoryUsage()));

		totalMemUsage += directPhotons.size() * sizeof(Photon) + directPhotonsBVH->GetMemoryUsage();
	}

	if (indirectPhotonsBVH) {
		SLG_LOG("PhotonGI indirect cache photons memory usage: " << ToMemString(indirectPhotons.size() * sizeof(Photon)));
		SLG_LOG("PhotonGI indirect cache BVH memory usage: " << ToMemString(indirectPhotonsBVH->GetMemoryUsage()));

		totalMemUsage += indirectPhotons.size() * sizeof(Photon) + indirectPhotonsBVH->GetMemoryUsage();
	}

	if (causticPhotonsBVH) {
		SLG_LOG("PhotonGI caustic cache photons memory usage: " << ToMemString(causticPhotons.size() * sizeof(Photon)));
		SLG_LOG("PhotonGI caustic cache BVH memory usage: " << ToMemString(causticPhotonsBVH->GetMemoryUsage()));

		totalMemUsage += causticPhotons.size() * sizeof(Photon) + causticPhotonsBVH->GetMemoryUsage();
	}

	if (radiancePhotonsBVH) {
		SLG_LOG("PhotonGI radiance cache photons memory usage: " << ToMemString(radiancePhotons.size() * sizeof(RadiancePhoton)));
		SLG_LOG("PhotonGI radiance cache BVH memory usage: " << ToMemString(radiancePhotonsBVH->GetMemoryUsage()));

		totalMemUsage += radiancePhotons.size() * sizeof(Photon) + radiancePhotonsBVH->GetMemoryUsage();
	}

	SLG_LOG("PhotonGI total memory usage: " << ToMemString(totalMemUsage));
}

Spectrum PhotonGICache::GetAllRadiance(const BSDF &bsdf) const {
	assert (bsdf.IsPhotonGIEnabled());

	Spectrum result;
	if (radiancePhotonsBVH) {
		vector<NearPhoton> entries;
		entries.reserve(radiancePhotonsBVH->GetEntryMaxLookUpCount());

		// Flip the normal if required
		const Normal n = (bsdf.hitPoint.intoObject ? 1.f: -1.f) * bsdf.hitPoint.shadeN;
		float maxDistance2;
		radiancePhotonsBVH->GetAllNearEntries(entries, bsdf.hitPoint.p, n, maxDistance2);

		if (entries.size() > 0) {
			for (auto const &nearPhoton : entries) {
				const RadiancePhoton *radiancePhoton = (const RadiancePhoton *)nearPhoton.photon;

				// Using a box filter here
				result += radiancePhoton->outgoingRadiance;
			}

			result /= entries.size();
		}
	}

	return result;
}

// Simpson filter from PBRT v2. Filter the photons according their
// distance, giving more weight to the nearest.
static inline float SimpsonKernel(const Point &p1, const Point &p2,
		const float maxDist2) {
	const float dist2 = DistanceSquared(p1, p2);

	// The distance between p1 and p2 is supposed to be < maxDist2
	assert (dist2 <= maxDist2);
    const float s = (1.f - dist2 / maxDist2);

    return 3.f * INV_PI * s * s;
}

Spectrum PhotonGICache::ProcessCacheEntries(const vector<NearPhoton> &entries,
		const u_int photonTracedCount, const float maxDistance2, const BSDF &bsdf) const {
	Spectrum result;

	if (entries.size() > 0) {
		if (bsdf.GetMaterialType() == MaterialType::MATTE) {
			// A fast path for matte material

			for (auto const &nearPhoton : entries) {
				const Photon *photon = (const Photon *)nearPhoton.photon;

				// Using a Simpson filter here
				result += SimpsonKernel(bsdf.hitPoint.p, photon->p, maxDistance2) * 
						AbsDot(bsdf.hitPoint.shadeN, -photon->d) * photon->alpha;
			}
			
			result *= bsdf.EvaluateTotal() * INV_PI;
		} else {
			// Generic path

			BSDFEvent event;
			for (auto const &nearPhoton : entries) {
				const Photon *photon = (const Photon *)nearPhoton.photon;

				// Using a Simpson filter here
				result += SimpsonKernel(bsdf.hitPoint.p, photon->p, maxDistance2) *
						bsdf.Evaluate(-photon->d, &event, nullptr, nullptr) * photon->alpha;
			}
		}
	}
	
	result /= photonTracedCount * maxDistance2;

	return result;
}

Spectrum PhotonGICache::GetDirectRadiance(const BSDF &bsdf) const {
	assert (bsdf.IsPhotonGIEnabled());

	if (directPhotonsBVH) {
		vector<NearPhoton> entries;
		entries.reserve(directPhotonsBVH->GetEntryMaxLookUpCount());

		// Flip the normal if required
		const Normal n = (bsdf.hitPoint.intoObject ? 1.f: -1.f) * bsdf.hitPoint.shadeN;
		float maxDistance2;
		directPhotonsBVH->GetAllNearEntries(entries, bsdf.hitPoint.p, n, maxDistance2);

		return ProcessCacheEntries(entries, directPhotonTracedCount, maxDistance2, bsdf);
	} else
		return Spectrum();
}

Spectrum PhotonGICache::GetIndirectRadiance(const BSDF &bsdf) const {
	assert (bsdf.IsPhotonGIEnabled());

	Spectrum result;
	if (radiancePhotonsBVH) {
		// Flip the normal if required
		const Normal n = (bsdf.hitPoint.intoObject ? 1.f: -1.f) * bsdf.hitPoint.shadeN;
		const RadiancePhoton *radiancePhoton = radiancePhotonsBVH->GetNearestEntry(bsdf.hitPoint.p, n);

		if (radiancePhoton)
			result = radiancePhoton->outgoingRadiance;
	}
	
	return result;
}

Spectrum PhotonGICache::GetCausticRadiance(const BSDF &bsdf) const {
	assert (bsdf.IsPhotonGIEnabled());

	if (causticPhotonsBVH) {
		vector<NearPhoton> entries;
		entries.reserve(causticPhotonsBVH->GetEntryMaxLookUpCount());

		// Flip the normal if required
		const Normal n = (bsdf.hitPoint.intoObject ? 1.f: -1.f) * bsdf.hitPoint.shadeN;
		float maxDistance2;
		causticPhotonsBVH->GetAllNearEntries(entries, bsdf.hitPoint.p, n, maxDistance2);

		return ProcessCacheEntries(entries, causticPhotonTracedCount, maxDistance2, bsdf);
	} else
		return Spectrum();
}

PhotonGISamplerType PhotonGICache::String2SamplerType(const string &type) {
	if (type == "RANDOM")
		return PhotonGISamplerType::PGIC_SAMPLER_RANDOM;
	else if (type == "METROPOLIS")
		return PhotonGISamplerType::PGIC_SAMPLER_METROPOLIS;
	else
		throw runtime_error("Unknown PhotonGI cache debug type: " + type);
}

string PhotonGICache::SamplerType2String(const PhotonGISamplerType type) {
	switch (type) {
		case PhotonGISamplerType::PGIC_SAMPLER_RANDOM:
			return "RANDOM";
		case PhotonGISamplerType::PGIC_SAMPLER_METROPOLIS:
			return "METROPOLIS";
		default:
			throw runtime_error("Unsupported wrap type in PhotonGICache::SamplerType2String(): " + ToString(type));
	}
}

PhotonGIDebugType PhotonGICache::String2DebugType(const string &type) {
	if (type == "showdirect")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWDIRECT;
	else if (type == "showindirect")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT;
	else if (type == "showcaustic")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC;
	else if (type == "none")
		return PhotonGIDebugType::PGIC_DEBUG_NONE;
	else
		throw runtime_error("Unknown PhotonGI cache debug type: " + type);
}

string PhotonGICache::DebugType2String(const PhotonGIDebugType type) {
	switch (type) {
		case PhotonGIDebugType::PGIC_DEBUG_SHOWDIRECT:
			return "showdirect";
		case PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT:
			return "showindirect";
		case PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC:
			return "showcaustic";
		case PhotonGIDebugType::PGIC_DEBUG_NONE:
			return "none";
		default:
			throw runtime_error("Unsupported wrap type in PhotonGICache::DebugType2String(): " + ToString(type));
	}
}

Properties PhotonGICache::ToProperties(const Properties &cfg) {
	Properties props;

	props <<
			cfg.Get(GetDefaultProps().Get("path.photongi.sampler.type")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.photon.maxcount")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.photon.maxdepth")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.visibility.enabled")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.visibility.targethitrate")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.visibility.maxsamplecount")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.visibility.lookup.radius")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.visibility.lookup.normalangle")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.direct.enabled")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.direct.maxsize")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.maxcount")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.radius")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.normalangle")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.indirect.enabled")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.indirect.maxsize")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.maxcount")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.radius")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.normalangle")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.caustic.enabled")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.caustic.maxsize")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.maxcount")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.radius")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.normalangle")) <<
			cfg.Get(GetDefaultProps().Get("path.photongi.debug.type"));

	return props;
}

const Properties &PhotonGICache::GetDefaultProps() {
	static Properties props = Properties() <<
			Property("path.photongi.sampler.type")("METROPOLIS") <<
			Property("path.photongi.photon.maxcount")(500000) <<
			Property("path.photongi.photon.maxdepth")(4) <<
			Property("path.photongi.visibility.enabled")(true) <<
			Property("path.photongi.visibility.targethitrate")(.99f) <<
			Property("path.photongi.visibility.maxsamplecount")(1024 * 1024) <<
			Property("path.photongi.visibility.lookup.radius")(.15f) <<
			Property("path.photongi.visibility.lookup.normalangle")(10.f) <<
			Property("path.photongi.direct.enabled")(false) <<
			Property("path.photongi.direct.maxsize")(25000) <<
			Property("path.photongi.direct.lookup.maxcount")(64) <<
			Property("path.photongi.direct.lookup.radius")(.15f) <<
			Property("path.photongi.direct.lookup.normalangle")(10.f) <<
			Property("path.photongi.indirect.enabled")(false) <<
			Property("path.photongi.indirect.maxsize")(100000) <<
			Property("path.photongi.indirect.lookup.maxcount")(64) <<
			Property("path.photongi.indirect.lookup.radius")(.15f) <<
			Property("path.photongi.indirect.lookup.normalangle")(10.f) <<
			Property("path.photongi.caustic.enabled")(false) <<
			Property("path.photongi.caustic.maxsize")(100000) <<
			Property("path.photongi.caustic.lookup.maxcount")(256) <<
			Property("path.photongi.caustic.lookup.radius")(.15f) <<
			Property("path.photongi.caustic.lookup.normalangle")(10.f) <<
			Property("path.photongi.debug.type")("none");

	return props;
}

PhotonGICache *PhotonGICache::FromProperties(const Scene *scn, const Properties &cfg) {
	PhotonGICacheParams params;

	params.direct.enabled = cfg.Get(GetDefaultProps().Get("path.photongi.direct.enabled")).Get<bool>();
	params.indirect.enabled = cfg.Get(GetDefaultProps().Get("path.photongi.indirect.enabled")).Get<bool>();
	params.caustic.enabled = cfg.Get(GetDefaultProps().Get("path.photongi.caustic.enabled")).Get<bool>();
	
	if (params.direct.enabled || params.indirect.enabled || params.caustic.enabled) {
		params.samplerType = String2SamplerType(cfg.Get(GetDefaultProps().Get("path.photongi.sampler.type")).Get<string>());

		params.photon.maxTracedCount = Max(1u, cfg.Get(GetDefaultProps().Get("path.photongi.photon.maxcount")).Get<u_int>());
		params.photon.maxPathDepth = Max(1u, cfg.Get(GetDefaultProps().Get("path.photongi.photon.maxdepth")).Get<u_int>());

		if (params.samplerType == PGIC_SAMPLER_METROPOLIS) {
			params.visibility.enabled = cfg.Get(GetDefaultProps().Get("path.photongi.visibility.enabled")).Get<bool>();
			params.visibility.targetHitRate = cfg.Get(GetDefaultProps().Get("path.photongi.visibility.targethitrate")).Get<float>();
			params.visibility.maxSampleCount = cfg.Get(GetDefaultProps().Get("path.photongi.visibility.maxsamplecount")).Get<u_int>();
			params.visibility.lookUpRadius = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.visibility.lookup.radius")).Get<float>());
			params.visibility.lookUpNormalAngle = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.visibility.lookup.normalangle")).Get<float>());
		} else
			params.visibility.enabled = false;

		if (params.direct.enabled) {
			params.direct.maxSize = Max(0u, cfg.Get(GetDefaultProps().Get("path.photongi.direct.maxsize")).Get<u_int>());

			params.direct.lookUpMaxCount = Max(1u, cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.maxcount")).Get<u_int>());
			params.direct.lookUpRadius = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.radius")).Get<float>());
			params.direct.lookUpNormalAngle = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.direct.lookup.normalangle")).Get<float>());
		}

		if (params.indirect.enabled) {
			params.indirect.maxSize = Max(0u, cfg.Get(GetDefaultProps().Get("path.photongi.indirect.maxsize")).Get<u_int>());

			params.indirect.lookUpMaxCount = Max(1u, cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.maxcount")).Get<u_int>());
			params.indirect.lookUpRadius = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.radius")).Get<float>());
			params.indirect.lookUpNormalAngle = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.indirect.lookup.normalangle")).Get<float>());
		}

		if (params.caustic.enabled) {
			params.caustic.maxSize = Max(0u, cfg.Get(GetDefaultProps().Get("path.photongi.caustic.maxsize")).Get<u_int>());

			params.caustic.lookUpMaxCount = Max(1u, cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.maxcount")).Get<u_int>());
			params.caustic.lookUpRadius = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.radius")).Get<float>());
			params.caustic.lookUpNormalAngle = Max(DEFAULT_EPSILON_MIN, cfg.Get(GetDefaultProps().Get("path.photongi.caustic.lookup.normalangle")).Get<float>());
		}

		params.debugType = String2DebugType(cfg.Get(GetDefaultProps().Get("path.photongi.debug.type")).Get<string>());

		return new PhotonGICache(scn, params);
	} else
		return nullptr;
}
