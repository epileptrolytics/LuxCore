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

#include "slg/scene/scene.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/caches/photongi/tracephotonsthread.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// TracePhotonsThread
//------------------------------------------------------------------------------

TracePhotonsThread::TracePhotonsThread(PhotonGICache &cache, const u_int index) :
	pgic(cache), threadIndex(index), renderThread(nullptr) {
}

TracePhotonsThread::~TracePhotonsThread() {
	Join();
}

void TracePhotonsThread::Start() {
	directPhotons.clear();
	indirectPhotons.clear();
	causticPhotons.clear();

	renderThread = new boost::thread(&TracePhotonsThread::RenderFunc, this);
}

void TracePhotonsThread::Join() {
	if (renderThread) {
		renderThread->join();

		delete renderThread;
		renderThread = nullptr;
	}
}

void TracePhotonsThread::UniformMutate(RandomGenerator &rndGen, vector<float> &samples) const {
	for (auto &sample: samples)
		sample = rndGen.floatValue();
}

void TracePhotonsThread::Mutate(RandomGenerator &rndGen,
		const vector<float> &currentPathSamples,
		vector<float> &candidatePathSamples,
		const float mutationSize) const {
	assert (candidatePathSamples.size() == currentPathSamples.size());
	assert (mutationSize != 0.f);

	for (u_int i = 0; i < currentPathSamples.size(); ++i) {
		const float deltaU = powf(rndGen.floatValue(), 1.f / mutationSize + 1.f);
		
		float mutateValue = currentPathSamples[i];
		if (rndGen.floatValue() < .5f) {
			mutateValue += deltaU;
			mutateValue = (mutateValue < 1.f) ? mutateValue : (mutateValue - 1.f);
		} else {
			mutateValue -= deltaU;
			mutateValue = (mutateValue < 0.f) ? (mutateValue + 1.f) : mutateValue;
		}
		
		// mutateValue can still be 1.f due to numerical precision problems
		candidatePathSamples[i] = (mutateValue == 1.f) ? 0.f : mutateValue;

		assert ((candidatePathSamples[i] >= 0.f) && (candidatePathSamples[i] < 1.f));
	}
}

bool TracePhotonsThread::TracePhotonPath(RandomGenerator &rndGen,
		const vector<float> &samples,
		vector<Photon> &newDirectPhotons,
		vector<Photon> &newIndirectPhotons,
		vector<Photon> &newCausticPhotons,
		vector<RadiancePhoton> &newRadiancePhotons) {
	newDirectPhotons.clear();
	newIndirectPhotons.clear();
	newCausticPhotons.clear();
	newRadiancePhotons.clear();
	
	const Scene *scene = pgic.scene;
	const Camera *camera = scene->camera;

	bool usefulPath = false;
	
	Spectrum lightPathFlux;
	lightPathFlux = Spectrum();

	const float timeSample = samples[0];
	const float time = camera->GenerateRayTime(timeSample);

	// Select one light source
	float lightPickPdf;
	const LightSource *light = scene->lightDefs.GetEmitLightStrategy()->
			SampleLights(samples[1], &lightPickPdf);

	if (light) {
		// Initialize the light path
		float lightEmitPdfW;
		Ray nextEventRay;
		lightPathFlux = light->Emit(*scene,
			samples[2], samples[3], samples[4], samples[5], samples[6],
				&nextEventRay.o, &nextEventRay.d, &lightEmitPdfW);
		nextEventRay.UpdateMinMaxWithEpsilon();
		nextEventRay.time = time;

		if (!lightPathFlux.Black()) {
			lightPathFlux /= lightEmitPdfW * lightPickPdf;
			assert (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

			//------------------------------------------------------------------
			// Trace the light path
			//------------------------------------------------------------------

			bool specularPath = true;
			u_int depth = 1;
			PathVolumeInfo volInfo;
			while (depth <= pgic.params.photon.maxPathDepth) {
				const u_int sampleOffset = sampleBootSize +	(depth - 1) * sampleStepSize;

				RayHit nextEventRayHit;
				BSDF bsdf;
				Spectrum connectionThroughput;
				const bool hit = scene->Intersect(nullptr, true, false, &volInfo, samples[sampleOffset],
						&nextEventRay, &nextEventRayHit, &bsdf,
						&connectionThroughput);

				if (hit) {
					// Something was hit

					lightPathFlux *= connectionThroughput;

					//----------------------------------------------------------
					// Deposit photons only on diffuse surfaces
					//----------------------------------------------------------

					if (bsdf.IsPhotonGIEnabled()) {
						// Flip the normal if required
						const Normal landingSurfaceNormal = ((Dot(bsdf.hitPoint.shadeN, -nextEventRay.d) > 0.f) ?
							1.f : -1.f) * bsdf.hitPoint.shadeN;

						bool visiblePoint = true;
						if (pgic.visibilityParticlesOctree) {
							// Check if the point is visible
							const u_int entryIndex = pgic.visibilityParticlesOctree->GetNearestEntry(bsdf.hitPoint.p, landingSurfaceNormal);
							
							visiblePoint = (entryIndex != NULL_INDEX);
						}

						if (visiblePoint) {
							bool usedPhoton = false;
							if ((depth == 1) && (pgic.params.direct.enabled || pgic.params.indirect.enabled)) {
								// It is a direct light photon
								if (!directDone) {
									newDirectPhotons.push_back(Photon(bsdf.hitPoint.p, nextEventRay.d,
											lightPathFlux, landingSurfaceNormal));
									usedPhoton = true;
								}

								usefulPath = true;
							} else if ((depth > 1) && specularPath && pgic.params.caustic.enabled) {
								// It is a caustic photon
								if (!causticDone) {
									newCausticPhotons.push_back(Photon(bsdf.hitPoint.p, nextEventRay.d,
											lightPathFlux, landingSurfaceNormal));
									usedPhoton = true;
								}

								usefulPath = true;
							} else if (pgic.params.indirect.enabled) {
								// It is an indirect photon
								if (!indirectDone) {
									newIndirectPhotons.push_back(Photon(bsdf.hitPoint.p, nextEventRay.d,
											lightPathFlux, landingSurfaceNormal));
									usedPhoton = true;
								}

								usefulPath = true;
							} 

							// Decide if to deposit a radiance photon
							if (usedPhoton && pgic.params.indirect.enabled && (rndGen.floatValue() > .1f)) {
								// I save the bsdf.EvaluateTotal() for later usage while
								// the radiance photon values are computed.

								newRadiancePhotons.push_back(RadiancePhoton(bsdf.hitPoint.p,
										landingSurfaceNormal, bsdf.EvaluateTotal()));
							}
						}
					}

					if (depth >= pgic.params.photon.maxPathDepth)
						break;

					//----------------------------------------------------------
					// Build the next vertex path ray
					//----------------------------------------------------------

					float bsdfPdf;
					Vector sampledDir;
					BSDFEvent event;
					float cosSampleDir;
					const Spectrum bsdfSample = bsdf.Sample(&sampledDir,
							samples[sampleOffset + 2],
							samples[sampleOffset + 3],
							&bsdfPdf, &cosSampleDir, &event);
					if (bsdfSample.Black())
						break;

					// Is it still a specular path ?
					specularPath = specularPath && (event & SPECULAR);

					lightPathFlux *= bsdfSample;
					assert (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

					// Update volume information
					volInfo.Update(event, bsdf);

					nextEventRay.Update(bsdf.hitPoint.p, sampledDir);
					++depth;
				} else {
					// Ray lost in space...
					break;
				}
			}
		}
	}

	return usefulPath;
}

void TracePhotonsThread::AddPhotons(const vector<Photon> &newDirectPhotons,
		const vector<Photon> &newIndirectPhotons,
		const vector<Photon> &newCausticPhotons,
		const vector<RadiancePhoton> &newRadiancePhotons) {
	directPhotons.insert(directPhotons.end(), newDirectPhotons.begin(),
			newDirectPhotons.end());
	indirectPhotons.insert(indirectPhotons.end(), newIndirectPhotons.begin(),
			newIndirectPhotons.end());
	causticPhotons.insert(causticPhotons.end(), newCausticPhotons.begin(),
			newCausticPhotons.end());
	radiancePhotons.insert(radiancePhotons.end(), newRadiancePhotons.begin(),
			newRadiancePhotons.end());	
}

void TracePhotonsThread::AddPhotons(const float currentPhotonsScale,
		const vector<Photon> &newDirectPhotons,
		const vector<Photon> &newIndirectPhotons,
		const vector<Photon> &newCausticPhotons,
		const vector<RadiancePhoton> &newRadiancePhotons) {
	for (auto const &photon : newDirectPhotons) {
		directPhotons.push_back(photon);
		directPhotons.back().alpha *= currentPhotonsScale;
	}

	for (auto const &photon : newIndirectPhotons) {
		indirectPhotons.push_back(photon);
		indirectPhotons.back().alpha *= currentPhotonsScale;
	}
	
	for (auto const &photon : newCausticPhotons) {
		causticPhotons.push_back(photon);
		causticPhotons.back().alpha *= currentPhotonsScale;
	}

	// Nothing to scale for radiance photons
	radiancePhotons.insert(radiancePhotons.end(), newRadiancePhotons.begin(),
			newRadiancePhotons.end());	
}

// The metropolis sampler used here is based on:
//  "Robust Adaptive Photon Tracing using Photon Path Visibility"
//  by TOSHIYA HACHISUKA and HENRIK WANN JENSEN

void TracePhotonsThread::RenderFunc() {
	const u_int workSize = 4096;

	//--------------------------------------------------------------------------
	// Initialization
	//--------------------------------------------------------------------------

	RandomGenerator rndGen(1 + threadIndex);

	sampleBootSize = 7;
	sampleStepSize = 4;
	sampleSize = 
			sampleBootSize + // To generate the initial setup
			pgic.params.photon.maxPathDepth * sampleStepSize; // For each light vertex

	vector<float> currentPathSamples(sampleSize);
	vector<float> candidatePathSamples(sampleSize);
	vector<float> uniformPathSamples(sampleSize);

	vector<Photon> currentDirectPhotons, currentIndirectPhotons, currentCausticPhotons;
	vector<RadiancePhoton> currentRadiancePhotons;
	vector<Photon> candidateDirectPhotons, candidateIndirectPhotons, candidateCausticPhotons;
	vector<RadiancePhoton> candidateRadiancePhotons;
	vector<Photon> uniformDirectPhotons, uniformIndirectPhotons, uniformCausticPhotons;
	vector<RadiancePhoton> uniformRadiancePhotons;

	//--------------------------------------------------------------------------
	// Get a bucket of work to do
	//--------------------------------------------------------------------------

	const double startTime = WallClockTime();
	double lastPrintTime = WallClockTime();
	while(!boost::this_thread::interruption_requested()) {
		// Get some work to do
		u_int workCounter;
		do {
			workCounter = pgic.globalPhotonsCounter;
		} while (!pgic.globalPhotonsCounter.compare_exchange_weak(workCounter, workCounter + workSize));

		// Check if it is time to stop
		if (workCounter >= pgic.params.photon.maxTracedCount)
			break;

		directDone = (pgic.globalDirectSize >= pgic.params.direct.maxSize);
		indirectDone = (pgic.globalIndirectSize >= pgic.params.indirect.maxSize);
		causticDone = (pgic.globalCausticSize >= pgic.params.caustic.maxSize);

		u_int workToDo = (workCounter + workSize > pgic.params.photon.maxTracedCount) ?
			(pgic.params.photon.maxTracedCount - workCounter) : workSize;

		if (!directDone)
			pgic.globalDirectPhotonsTraced += workToDo;
		if (!indirectDone)
			pgic.globalIndirectPhotonsTraced += workToDo;
		if (!causticDone)
			pgic.globalCausticPhotonsTraced += workToDo;

		// Print some progress information
		if (threadIndex == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				const float directProgress = pgic.params.direct.enabled ?
					((pgic.globalDirectSize > 0) ? ((100.0 * pgic.globalDirectSize) / pgic.params.direct.maxSize) : 0.f) :
					100.f;
				const float indirectProgress = pgic.params.indirect.enabled ?
					((pgic.globalIndirectSize > 0) ? ((100.0 * pgic.globalIndirectSize) / pgic.params.indirect.maxSize) : 0.f) :
					100.f;
				const float causticProgress = pgic.params.caustic.enabled ?
					((pgic.globalCausticSize > 0) ? ((100.0 * pgic.globalCausticSize) / pgic.params.caustic.maxSize) : 0.f) :
					100.f;

				SLG_LOG(boost::format("PhotonGI Cache photon traced: %d/%d [%.1f%%, %.1fM photons/sec, Map sizes (%.1f%%, %.1f%%, %.1f%%)]") %
						workCounter % pgic.params.photon.maxTracedCount %
						((100.0 * workCounter) / pgic.params.photon.maxTracedCount) %
						(workCounter / (1000.0 * (WallClockTime() - startTime))) %
						directProgress %
						indirectProgress %
						causticProgress);
				lastPrintTime = now;
			}
		}

		const u_int directPhotonsStart = directPhotons.size();
		const u_int indirectPhotonsStart = indirectPhotons.size();
		const u_int causticPhotonsStart = causticPhotons.size();

		//----------------------------------------------------------------------
		// Metropolis Sampler
		//----------------------------------------------------------------------

		if (pgic.params.samplerType == PGIC_SAMPLER_METROPOLIS) {
			// Look for a useful path to start with

			bool foundUseful = false;
			for (u_int i = 0; i < 16384; ++i) {
				UniformMutate(rndGen, currentPathSamples);

				foundUseful = TracePhotonPath(rndGen, currentPathSamples, currentDirectPhotons,
						currentIndirectPhotons, currentCausticPhotons, currentRadiancePhotons);
				if (foundUseful)
					break;

#ifdef WIN32
				// Work around Windows bad scheduling
				renderThread->yield();
#endif
			}

			if (!foundUseful) {
				// I was unable to find a useful path. Something wrong. this
				// may be an empty scene.
				throw runtime_error("Unable to find a useful path in TracePhotonsThread::RenderFunc()");
			}

			// Trace light paths

			u_int currentPhotonsScale = 1;
			float mutationSize = 1.f;
			u_int acceptedCount = 1;
			u_int mutatedCount = 1;
			u_int uniformCount = 1;
			u_int workToDoIndex = workToDo;
			while (workToDoIndex-- && !boost::this_thread::interruption_requested()) {
				UniformMutate(rndGen, uniformPathSamples);

				if (TracePhotonPath(rndGen, uniformPathSamples, uniformDirectPhotons,
						uniformIndirectPhotons, uniformCausticPhotons, uniformRadiancePhotons)) {
					// Add the old current photons (scaled by currentPhotonsScale)
					AddPhotons(currentPhotonsScale, currentDirectPhotons, currentIndirectPhotons, currentCausticPhotons,
							currentRadiancePhotons);
					
					// The candidate path becomes the current one
					copy(uniformPathSamples.begin(), uniformPathSamples.end(), currentPathSamples.begin());

					currentPhotonsScale = 1;
					currentDirectPhotons = uniformDirectPhotons;
					currentIndirectPhotons = uniformIndirectPhotons;
					currentCausticPhotons = uniformCausticPhotons;
					currentRadiancePhotons = uniformRadiancePhotons;

					++uniformCount;
				} else {
					// Try a mutation of the current path
					Mutate(rndGen, currentPathSamples, candidatePathSamples, mutationSize);
					++mutatedCount;

					if (TracePhotonPath(rndGen, candidatePathSamples, candidateDirectPhotons,
							candidateIndirectPhotons, candidateCausticPhotons, candidateRadiancePhotons)) {
						// Add the old current photons (scaled by currentPhotonsScale)
						AddPhotons(currentPhotonsScale, currentDirectPhotons, currentIndirectPhotons, currentCausticPhotons,
								currentRadiancePhotons);

						// The candidate path becomes the current one
						copy(candidatePathSamples.begin(), candidatePathSamples.end(), currentPathSamples.begin());

						currentPhotonsScale = 1;
						currentDirectPhotons = candidateDirectPhotons;
						currentIndirectPhotons = candidateIndirectPhotons;
						currentCausticPhotons = candidateCausticPhotons;
						currentRadiancePhotons = candidateRadiancePhotons;

						++acceptedCount;
					} else
						++currentPhotonsScale;

					const float R = acceptedCount / (float)mutatedCount;
					// 0.234 => the optimal asymptotic acceptance ratio has been
					// derived 23.4% [Roberts et al. 1997]
					mutationSize += (R - .234f) / mutatedCount;
				}
				
#ifdef WIN32
				// Work around Windows bad scheduling
				renderThread->yield();
#endif
			}

			// Add the last current photons (scaled by currentPhotonsScale)
			if (currentPhotonsScale > 1) {
				AddPhotons(currentPhotonsScale, currentDirectPhotons, currentIndirectPhotons, currentCausticPhotons,
						currentRadiancePhotons);
			}

			// Scale all photon values
			const float scaleFactor = uniformCount /  (float)workToDo;

			for (u_int i = directPhotonsStart; i < directPhotons.size(); ++i)
				directPhotons[i].alpha *= scaleFactor;
			for (u_int i = indirectPhotonsStart; i < indirectPhotons.size(); ++i)
				indirectPhotons[i].alpha *= scaleFactor;
			for (u_int i = causticPhotonsStart; i < causticPhotons.size(); ++i)
				causticPhotons[i].alpha *= scaleFactor;
		} else

		//----------------------------------------------------------------------
		// Random Sampler
		//----------------------------------------------------------------------

		if (pgic.params.samplerType == PGIC_SAMPLER_RANDOM) {
			// Trace light paths

			u_int workToDoIndex = workToDo;
			while (workToDoIndex-- && !boost::this_thread::interruption_requested()) {
				UniformMutate(rndGen, currentPathSamples);

				TracePhotonPath(rndGen, currentPathSamples, currentDirectPhotons,
						currentIndirectPhotons, currentCausticPhotons, currentRadiancePhotons);

				// Add the new photons
				AddPhotons(currentDirectPhotons, currentIndirectPhotons, currentCausticPhotons,
						currentRadiancePhotons);

#ifdef WIN32
				// Work around Windows bad scheduling
				renderThread->yield();
#endif
			}
		} else
			throw runtime_error("Unknow sampler type in TracePhotonsThread::RenderFunc(): " + ToString(pgic.params.samplerType));
		
		//----------------------------------------------------------------------
		
		// Update size counters
		pgic.globalDirectSize += directPhotons.size() - directPhotonsStart;
		pgic.globalIndirectSize += indirectPhotons.size() - indirectPhotonsStart;
		pgic.globalCausticSize += causticPhotons.size() - causticPhotonsStart;

		// Check if it is time to stop. I can do the check only here because
		// globalPhotonsTraced was already incremented
		if (directDone && indirectDone && causticDone)
			break;
	}
}
