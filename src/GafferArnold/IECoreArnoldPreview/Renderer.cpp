//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2016, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "GafferScene/Private/IECoreScenePreview/Renderer.h"

#include "GafferArnold/Private/IECoreArnoldPreview/ShaderNetworkAlgo.h"

#include "GafferScene/Private/IECoreScenePreview/Procedural.h"

#include "IECoreArnold/CameraAlgo.h"
#include "IECoreArnold/NodeAlgo.h"
#include "IECoreArnold/ParameterAlgo.h"
#include "IECoreArnold/UniverseBlock.h"

#include "IECoreScene/Camera.h"
#include "IECoreScene/CurvesPrimitive.h"
#include "IECoreScene/ExternalProcedural.h"
#include "IECoreScene/MeshPrimitive.h"
#include "IECoreScene/Shader.h"
#include "IECoreScene/SpherePrimitive.h"
#include "IECoreScene/Transform.h"

#include "IECoreVDB/VDBObject.h"
#include "IECoreVDB/TypeIds.h"

#include "IECore/MessageHandler.h"
#include "IECore/SimpleTypedData.h"
#include "IECore/StringAlgo.h"
#include "IECore/VectorTypedData.h"

#include "boost/algorithm/string.hpp"
#include "boost/algorithm/string/join.hpp"
#include "boost/algorithm/string/predicate.hpp"
#include "boost/bind.hpp"
#include "boost/container/flat_map.hpp"
#include "boost/filesystem/operations.hpp"
#include "boost/format.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/optional.hpp"

#include "tbb/concurrent_unordered_map.h"
#include "tbb/concurrent_vector.h"
#include "tbb/partitioner.h"
#include "tbb/spin_mutex.h"

#include <memory>
#include <thread>
#include <functional>
#include <unordered_set>

using namespace std;
using namespace boost::filesystem;
using namespace IECoreArnold;
using namespace IECoreArnoldPreview;

//////////////////////////////////////////////////////////////////////////
// Utilities
//////////////////////////////////////////////////////////////////////////

namespace
{

typedef std::shared_ptr<AtNode> SharedAtNodePtr;
typedef bool (*NodeDeleter)( AtNode *);

bool nullNodeDeleter( AtNode *node )
{
	return false;
}

NodeDeleter nodeDeleter( IECoreScenePreview::Renderer::RenderType renderType )
{
	if( renderType == IECoreScenePreview::Renderer::Interactive )
	{
		// As interactive edits add/remove objects and shaders, we want to
		// destroy any AtNodes that are no longer needed.
		return AiNodeDestroy;
	}
	else
	{
		// Edits are not possible, so we have no need to delete nodes except
		// when shutting the renderer down. `AiEnd()` (as called by ~UniverseBlock)
		// automatically destroys all nodes and is _much_ faster than destroying
		// them one by one with AiNodeDestroy. So we use a null deleter so that we
		// don't try to destroy the nodes ourselves, and rely entirely on `AiEnd()`.
		return nullNodeDeleter;
	}
}

template<typename T>
T *reportedCast( const IECore::RunTimeTyped *v, const char *type, const IECore::InternedString &name )
{
	T *t = IECore::runTimeCast<T>( v );
	if( t )
	{
		return t;
	}

	IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer", boost::format( "Expected %s but got %s for %s \"%s\"." ) % T::staticTypeName() % v->typeName() % type % name.c_str() );
	return nullptr;
}

template<typename T>
T parameter( const IECore::CompoundDataMap &parameters, const IECore::InternedString &name, const T &defaultValue )
{
	IECore::CompoundDataMap::const_iterator it = parameters.find( name );
	if( it == parameters.end() )
	{
		return defaultValue;
	}

	typedef IECore::TypedData<T> DataType;
	if( const DataType *d = reportedCast<const DataType>( it->second.get(), "parameter", name ) )
	{
		return d->readable();
	}
	else
	{
		return defaultValue;
	}
}

std::string formatHeaderParameter( const std::string name, const IECore::Data *data )
{
	if( const IECore::BoolData *boolData = IECore::runTimeCast<const IECore::BoolData>( data ) )
	{
		return boost::str( boost::format( "int '%s' %i" ) % name % int(boolData->readable()) );
	}
	else if( const IECore::FloatData *floatData = IECore::runTimeCast<const IECore::FloatData>( data ) )
	{
		return boost::str( boost::format( "float '%s' %f" ) % name % floatData->readable() );
	}
	else if( const IECore::IntData *intData = IECore::runTimeCast<const IECore::IntData>( data ) )
	{
		return boost::str( boost::format( "int '%s' %i" ) % name % intData->readable() );
	}
	else if( const IECore::StringData *stringData = IECore::runTimeCast<const IECore::StringData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % stringData->readable() );
	}
	else if( const IECore::V2iData *v2iData = IECore::runTimeCast<const IECore::V2iData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % v2iData->readable() );
	}
	else if( const IECore::V3iData *v3iData = IECore::runTimeCast<const IECore::V3iData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % v3iData->readable() );
	}
	else if( const IECore::V2fData *v2fData = IECore::runTimeCast<const IECore::V2fData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % v2fData->readable() );
	}
	else if( const IECore::V3fData *v3fData = IECore::runTimeCast<const IECore::V3fData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % v3fData->readable() );
	}
	else if( const IECore::Color3fData *c3fData = IECore::runTimeCast<const IECore::Color3fData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % c3fData->readable() );
	}
	else if( const IECore::Color4fData *c4fData = IECore::runTimeCast<const IECore::Color4fData>( data ) )
	{
		return boost::str( boost::format( "string '%s' %s" ) % name % c4fData->readable() );
	}
	else
	{
		IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer", boost::format( "Cannot convert data \"%s\" of type \"%s\"." ) % name % data->typeName() );
		return "";
	}
}

bool aiVersionLessThan( int arch, int major, int minor, int patch )
{
	// The Arnold API has an `AiCheckAPIVersion()` function that sounds
	// like exactly what we need, but it doesn't support comparing for
	// patch versions. Instead we're forced to parse the version string
	// ourselves.

	const char *arnoldVersionString = AiGetVersion( nullptr, nullptr, nullptr, nullptr );
	int arnoldVersion[4];
	for( int i = 0; i < 4; ++i )
	{
		arnoldVersion[i] = strtol( arnoldVersionString, const_cast<char **>( &arnoldVersionString ), 10 );
		++arnoldVersionString;
	}

	auto version = { arch, major, minor, patch };

	return std::lexicographical_compare(
		begin( arnoldVersion ), end( arnoldVersion ),
		version.begin(), version.end()
	);
}

const AtString g_aaSamplesArnoldString( "AA_samples" );
const AtString g_aaSeedArnoldString( "AA_seed" );
const AtString g_aovShadersArnoldString( "aov_shaders" );
const AtString g_autoArnoldString( "auto" );
const AtString g_atmosphereArnoldString( "atmosphere" );
const AtString g_backgroundArnoldString( "background" );
const AtString g_boxArnoldString("box");
const AtString g_cameraArnoldString( "camera" );
const AtString g_catclarkArnoldString("catclark");
const AtString g_customAttributesArnoldString( "custom_attributes" );
const AtString g_curvesArnoldString("curves");
const AtString g_dispMapArnoldString( "disp_map" );
const AtString g_dispHeightArnoldString( "disp_height" );
const AtString g_dispPaddingArnoldString( "disp_padding" );
const AtString g_dispZeroValueArnoldString( "disp_zero_value" );
const AtString g_dispAutoBumpArnoldString( "disp_autobump" );
const AtString g_fileNameArnoldString( "filename" );
const AtString g_filtersArnoldString( "filters" );
const AtString g_funcPtrArnoldString( "funcptr" );
const AtString g_ginstanceArnoldString( "ginstance" );
const AtString g_lightGroupArnoldString( "light_group" );
const AtString g_shadowGroupArnoldString( "shadow_group" );
const AtString g_linearArnoldString( "linear" );
const AtString g_matrixArnoldString( "matrix" );
const AtString g_geometryMatrixArnoldString( "geometry_matrix" );
const AtString g_matteArnoldString( "matte" );
const AtString g_meshArnoldString( "mesh" );
const AtString g_modeArnoldString( "mode" );
const AtString g_minPixelWidthArnoldString( "min_pixel_width" );
const AtString g_meshLightArnoldString("mesh_light");
const AtString g_motionStartArnoldString( "motion_start" );
const AtString g_motionEndArnoldString( "motion_end" );
const AtString g_nameArnoldString( "name" );
const AtString g_nodeArnoldString("node");
const AtString g_objectArnoldString( "object" );
const AtString g_opaqueArnoldString( "opaque" );
const AtString g_proceduralArnoldString( "procedural" );
const AtString g_pinCornersArnoldString( "pin_corners" );
const AtString g_pixelAspectRatioArnoldString( "pixel_aspect_ratio" );
const AtString g_pluginSearchPathArnoldString( "plugin_searchpath" );
const AtString g_polymeshArnoldString("polymesh");
const AtString g_rasterArnoldString( "raster" );
const AtString g_receiveShadowsArnoldString( "receive_shadows" );
const AtString g_regionMinXArnoldString( "region_min_x" );
const AtString g_regionMaxXArnoldString( "region_max_x" );
const AtString g_regionMinYArnoldString( "region_min_y" );
const AtString g_regionMaxYArnoldString( "region_max_y" );
const AtString g_selfShadowsArnoldString( "self_shadows" );
const AtString g_shaderArnoldString( "shader" );
const AtString g_shutterStartArnoldString( "shutter_start" );
const AtString g_shutterEndArnoldString( "shutter_end" );
const AtString g_sidednessArnoldString( "sidedness" );
const AtString g_sphereArnoldString("sphere");
const AtString g_sssSetNameArnoldString( "sss_setname" );
const AtString g_stepSizeArnoldString( "step_size" );
const AtString g_stepScaleArnoldString( "step_scale" );
const AtString g_subdivIterationsArnoldString( "subdiv_iterations" );
const AtString g_subdivAdaptiveErrorArnoldString( "subdiv_adaptive_error" );
const AtString g_subdivAdaptiveMetricArnoldString( "subdiv_adaptive_metric" );
const AtString g_subdivAdaptiveSpaceArnoldString( "subdiv_adaptive_space" );
const AtString g_subdivSmoothDerivsArnoldString( "subdiv_smooth_derivs" );
const AtString g_subdivTypeArnoldString( "subdiv_type" );
const AtString g_subdivUVSmoothingArnoldString( "subdiv_uv_smoothing" );
const AtString g_traceSetsArnoldString( "trace_sets" );
const AtString g_transformTypeArnoldString( "transform_type" );
const AtString g_thickArnoldString( "thick" );
const AtString g_useLightGroupArnoldString( "use_light_group" );
const AtString g_useShadowGroupArnoldString( "use_shadow_group" );
const AtString g_userPtrArnoldString( "userptr" );
const AtString g_visibilityArnoldString( "visibility" );
const AtString g_volumeArnoldString("volume");
const AtString g_volumePaddingArnoldString( "volume_padding" );
const AtString g_volumeGridsArnoldString( "grids" );
const AtString g_velocityGridsArnoldString( "velocity_grids" );
const AtString g_velocityScaleArnoldString( "velocity_scale" );
const AtString g_velocityFPSArnoldString( "velocity_fps" );
const AtString g_velocityOutlierThresholdArnoldString( "velocity_outlier_threshold" );
const AtString g_widthArnoldString( "width" );
const AtString g_xresArnoldString( "xres" );
const AtString g_yresArnoldString( "yres" );
const AtString g_filterMapArnoldString( "filtermap" );
const AtString g_uvRemapArnoldString( "uv_remap" );

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldRendererBase forward declaration
//////////////////////////////////////////////////////////////////////////

namespace
{

class ArnoldGlobals;
class Instance;
IE_CORE_FORWARDDECLARE( ShaderCache );
IE_CORE_FORWARDDECLARE( InstanceCache );
IE_CORE_FORWARDDECLARE( LightListCache );
IE_CORE_FORWARDDECLARE( ArnoldObject );
IE_CORE_FORWARDDECLARE( LightFilterConnections );

class ArnoldLight;
class ArnoldLightFilter;

/// This class implements the basics of outputting attributes
/// and objects to Arnold, but is not a complete implementation
/// of the renderer interface. It is subclassed to provide concrete
/// implementations suitable for use as the master renderer or
/// for use in procedurals.
class ArnoldRendererBase : public IECoreScenePreview::Renderer
{

	public :

		~ArnoldRendererBase() override;

		IECore::InternedString name() const override;

		Renderer::AttributesInterfacePtr attributes( const IECore::CompoundObject *attributes ) override;

		ObjectInterfacePtr camera( const std::string &name, const IECoreScene::Camera *camera, const AttributesInterface *attributes ) override;
		ObjectInterfacePtr light( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override;
		ObjectInterfacePtr lightFilter( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override;
		Renderer::ObjectInterfacePtr object( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override;
		ObjectInterfacePtr object( const std::string &name, const std::vector<const IECore::Object *> &samples, const std::vector<float> &times, const AttributesInterface *attributes ) override;

	protected :

		ArnoldRendererBase( NodeDeleter nodeDeleter, LightFilterConnectionsPtr lightFilterConnections, AtNode *parentNode = nullptr );

		NodeDeleter m_nodeDeleter;
		ShaderCachePtr m_shaderCache;
		InstanceCachePtr m_instanceCache;
		LightListCachePtr m_lightListCache;
		LightFilterConnectionsPtr m_connections;

	private :

		AtNode *m_parentNode;

};

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldOutput
//////////////////////////////////////////////////////////////////////////

namespace
{

class ArnoldOutput : public IECore::RefCounted
{

	public :

		ArnoldOutput( const IECore::InternedString &name, const IECoreScene::Output *output, NodeDeleter nodeDeleter )
		{
			// Create a driver node and set its parameters.

			AtString driverNodeType( output->getType().c_str() );
			if( AiNodeEntryGetType( AiNodeEntryLookUp( driverNodeType ) ) != AI_NODE_DRIVER )
			{
				// Automatically map tiff to driver_tiff and so on, to provide a degree of
				// compatibility with existing renderman driver names.
				AtString prefixedType( ( std::string("driver_") + driverNodeType.c_str() ).c_str() );
				if( AiNodeEntryLookUp( prefixedType ) )
				{
					driverNodeType = prefixedType;
				}
			}

			const std::string driverNodeName = boost::str( boost::format( "ieCoreArnold:display:%s" ) % name.string() );
			m_driver.reset(
				AiNode( driverNodeType, AtString( driverNodeName.c_str() ) ),
				nodeDeleter
			);
			if( !m_driver )
			{
				throw IECore::Exception( boost::str( boost::format( "Unable to create output driver of type \"%s\"" ) % driverNodeType.c_str() ) );
			}

			if( const AtParamEntry *fileNameParameter = AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( m_driver.get() ), g_fileNameArnoldString ) )
			{
				AiNodeSetStr( m_driver.get(), AiParamGetName( fileNameParameter ), AtString( output->getName().c_str() ) );
			}

			IECore::StringVectorDataPtr customAttributesData;
			if( const IECore::StringVectorData *d = output->parametersData()->member<IECore::StringVectorData>( "custom_attributes") )
			{
				customAttributesData = d->copy();
			}
			else
			{
				customAttributesData = new IECore::StringVectorData();
			}

			std::vector<std::string> &customAttributes = customAttributesData->writable();
			for( IECore::CompoundDataMap::const_iterator it = output->parameters().begin(), eIt = output->parameters().end(); it != eIt; ++it )
			{
				if( boost::starts_with( it->first.string(), "filter" ) )
				{
					continue;
				}

				if( boost::starts_with( it->first.string(), "header:" ) )
				{
					std::string formattedString = formatHeaderParameter( it->first.string().substr( 7 ), it->second.get() );
					if( !formattedString.empty())
					{
						customAttributes.push_back( formattedString );
					}
				}

				if( it->first.string() == "camera" )
				{
					if( const IECore::StringData *d = IECore::runTimeCast<const IECore::StringData>( it->second.get() ) )
					{
						m_cameraOverride = d->readable();
						continue;
					}
				}

				ParameterAlgo::setParameter( m_driver.get(), it->first.c_str(), it->second.get() );
			}

			if( AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( m_driver.get() ), g_customAttributesArnoldString ) )
			{
				ParameterAlgo::setParameter( m_driver.get(), "custom_attributes", customAttributesData.get() );
			}

			// Create a filter.

			std::string filterNodeType = parameter<std::string>( output->parameters(), "filter", "gaussian" );
			if( AiNodeEntryGetType( AiNodeEntryLookUp( AtString( filterNodeType.c_str() ) ) ) != AI_NODE_FILTER )
			{
				filterNodeType = filterNodeType + "_filter";
			}

			const std::string filterNodeName = boost::str( boost::format( "ieCoreArnold:filter:%s" ) % name.string() );
			m_filter.reset(
				AiNode( AtString( filterNodeType.c_str() ), AtString( filterNodeName.c_str() ) ),
				nodeDeleter
			);
			if( AiNodeEntryGetType( AiNodeGetNodeEntry( m_filter.get() ) ) != AI_NODE_FILTER )
			{
				throw IECore::Exception( boost::str( boost::format( "Unable to create filter of type \"%s\"" ) % filterNodeType ) );
			}

			for( IECore::CompoundDataMap::const_iterator it = output->parameters().begin(), eIt = output->parameters().end(); it != eIt; ++it )
			{
				if( !boost::starts_with( it->first.string(), "filter" ) || it->first == "filter" )
				{
					continue;
				}

				if( it->first == "filterwidth" )
				{
					// Special case to convert RenderMan style `float filterwidth[2]` into
					// Arnold style `float width`.
					if( const IECore::V2fData *v = IECore::runTimeCast<const IECore::V2fData>( it->second.get() ) )
					{
						if( v->readable().x != v->readable().y )
						{
							IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer", "Non-square filterwidth not supported" );
						}
						AiNodeSetFlt( m_filter.get(), g_widthArnoldString, v->readable().x );
						continue;
					}
				}

				ParameterAlgo::setParameter( m_filter.get(), it->first.c_str() + 6, it->second.get() );
			}

			// Convert the data specification to the form
			// supported by Arnold.

			m_data = output->getData();
			m_lpeName = "ieCoreArnold:lpe:" + name.string();
			m_lpeValue = "";

			if( m_data=="rgb" )
			{
				m_data = "RGB RGB";
			}
			else if( m_data=="rgba" )
			{
				m_data = "RGBA RGBA";
			}
			else
			{
				std::string arnoldType = "RGB";
				if( parameter<bool>( output->parameters(), "includeAlpha", false ) )
				{
					arnoldType = "RGBA";
				}

				vector<std::string> tokens;
				IECore::StringAlgo::tokenize( m_data, ' ', tokens );
				if( tokens.size() == 2 )
				{
					if( tokens[0] == "color" )
					{
						m_data = tokens[1] + " " + arnoldType;
					}
					else if( tokens[0] == "lpe" )
					{
						m_lpeValue = tokens[1];
						m_data = m_lpeName + " " + arnoldType;
					}
				}
			}
		}

		void append( std::vector<std::string> &outputs, std::vector<std::string> &lightPathExpressions ) const
		{
			outputs.push_back( boost::str( boost::format( "%s %s %s" ) % m_data % AiNodeGetName( m_filter.get() ) % AiNodeGetName( m_driver.get() ) ) );
			if( m_lpeValue.size() )
			{
				lightPathExpressions.push_back( m_lpeName + " " + m_lpeValue );
			}
		}

		const std::string &cameraOverride()
		{
			return m_cameraOverride;
		}

	private :

		SharedAtNodePtr m_driver;
		SharedAtNodePtr m_filter;
		std::string m_data;
		std::string m_lpeName;
		std::string m_lpeValue;
		std::string m_cameraOverride;

};

IE_CORE_DECLAREPTR( ArnoldOutput )

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldShader
//////////////////////////////////////////////////////////////////////////

namespace
{

class ArnoldShader : public IECore::RefCounted
{

	public :

		ArnoldShader( const IECoreScene::ShaderNetwork *shaderNetwork, NodeDeleter nodeDeleter, const std::string &namePrefix, const AtNode *parentNode )
			:	m_nodeDeleter( nodeDeleter )
		{
			m_nodes = ShaderNetworkAlgo::convert( shaderNetwork, namePrefix, parentNode );
		}

		~ArnoldShader() override
		{
			for( std::vector<AtNode *>::const_iterator it = m_nodes.begin(), eIt = m_nodes.end(); it != eIt; ++it )
			{
				m_nodeDeleter( *it );
			}
		}

		AtNode *root() const
		{
			return !m_nodes.empty() ? m_nodes.back() : nullptr;
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			nodes.insert( nodes.end(), m_nodes.begin(), m_nodes.end() );
		}

	private :

		NodeDeleter m_nodeDeleter;
		std::vector<AtNode *> m_nodes;

};

IE_CORE_DECLAREPTR( ArnoldShader )

class ShaderCache : public IECore::RefCounted
{

	public :

		ShaderCache( NodeDeleter nodeDeleter, AtNode *parentNode )
			:	m_nodeDeleter( nodeDeleter ), m_parentNode( parentNode )
		{
		}

		// Can be called concurrently with other get() calls.
		ArnoldShaderPtr get( const IECoreScene::ShaderNetwork *shader )
		{
			Cache::accessor a;
			m_cache.insert( a, shader->Object::hash() );
			if( !a->second )
			{
				const std::string namePrefix = "shader:" + a->first.toString() + ":";
				a->second = new ArnoldShader( shader, m_nodeDeleter, namePrefix, m_parentNode );
			}
			return a->second;
		}

		// Must not be called concurrently with anything.
		void clearUnused()
		{
			vector<IECore::MurmurHash> toErase;
			for( Cache::iterator it = m_cache.begin(), eIt = m_cache.end(); it != eIt; ++it )
			{
				if( it->second->refCount() == 1 )
				{
					// Only one reference - this is ours, so
					// nothing outside of the cache is using the
					// shader.
					toErase.push_back( it->first );
				}
			}
			for( vector<IECore::MurmurHash>::const_iterator it = toErase.begin(), eIt = toErase.end(); it != eIt; ++it )
			{
				m_cache.erase( *it );
			}
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			for( Cache::const_iterator it = m_cache.begin(), eIt = m_cache.end(); it != eIt; ++it )
			{
				it->second->nodesCreated( nodes );
			}
		}

	private :

		NodeDeleter m_nodeDeleter;
		AtNode *m_parentNode;

		typedef tbb::concurrent_hash_map<IECore::MurmurHash, ArnoldShaderPtr> Cache;
		Cache m_cache;
};

IE_CORE_DECLAREPTR( ShaderCache )


// The LightListCache stores std::vectors of AtNode pointers to lights.
// We rely on lights getting handled before other objects and if
// that were to change, we couldn't look up lights by name anymore.
class LightListCache : public IECore::RefCounted
{

	public :

		const std::vector<AtNode *> &get( const IECore::StringVectorData *nodeNamesData )
		{
			IECore::MurmurHash h = nodeNamesData->Object::hash();

			Cache::accessor a;
			m_cache.insert( a, h );

			if( a->second.empty() )
			{
				const std::vector<string> &nodeNames = nodeNamesData->readable();
				a->second.reserve( nodeNames.size() );

				for( const std::string &name : nodeNames )
				{
					std::string nodeName = "light:" + name;
					AtNode *node = AiNodeLookUpByName( AtString( nodeName.c_str() ) );
					if( node )
					{
						a->second.push_back( node );
					}
				}

				a->second.shrink_to_fit();
			}

			return a->second;
		}

		void clear()
		{
			m_cache.clear();
		}

	private :

		typedef tbb::concurrent_hash_map<IECore::MurmurHash, std::vector<AtNode *>> Cache;
		Cache m_cache;
};

IE_CORE_DECLAREPTR( LightListCache )

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldAttributes
//////////////////////////////////////////////////////////////////////////

namespace
{

IECore::InternedString g_surfaceShaderAttributeName( "surface" );
IECore::InternedString g_lightShaderAttributeName( "light" );
IECore::InternedString g_doubleSidedAttributeName( "doubleSided" );
IECore::InternedString g_setsAttributeName( "sets" );

IECore::InternedString g_oslSurfaceShaderAttributeName( "osl:surface" );
IECore::InternedString g_oslShaderAttributeName( "osl:shader" );

IECore::InternedString g_cameraVisibilityAttributeName( "ai:visibility:camera" );
IECore::InternedString g_shadowVisibilityAttributeName( "ai:visibility:shadow" );
IECore::InternedString g_shadowGroup( "ai:visibility:shadow_group" );
IECore::InternedString g_diffuseReflectVisibilityAttributeName( "ai:visibility:diffuse_reflect" );
IECore::InternedString g_specularReflectVisibilityAttributeName( "ai:visibility:specular_reflect" );
IECore::InternedString g_diffuseTransmitVisibilityAttributeName( "ai:visibility:diffuse_transmit" );
IECore::InternedString g_specularTransmitVisibilityAttributeName( "ai:visibility:specular_transmit" );
IECore::InternedString g_volumeVisibilityAttributeName( "ai:visibility:volume" );
IECore::InternedString g_subsurfaceVisibilityAttributeName( "ai:visibility:subsurface" );

IECore::InternedString g_arnoldSurfaceShaderAttributeName( "ai:surface" );
IECore::InternedString g_arnoldLightShaderAttributeName( "ai:light" );
IECore::InternedString g_arnoldFilterMapAttributeName( "ai:filtermap" );
IECore::InternedString g_arnoldUVRemapAttributeName( "ai:uv_remap" );
IECore::InternedString g_arnoldLightFilterShaderAttributeName( "ai:lightFilter:filter" );

IECore::InternedString g_arnoldReceiveShadowsAttributeName( "ai:receive_shadows" );
IECore::InternedString g_arnoldSelfShadowsAttributeName( "ai:self_shadows" );
IECore::InternedString g_arnoldOpaqueAttributeName( "ai:opaque" );
IECore::InternedString g_arnoldMatteAttributeName( "ai:matte" );

IECore::InternedString g_volumeStepSizeAttributeName( "ai:volume:step_size" );
IECore::InternedString g_volumeStepScaleAttributeName( "ai:volume:step_scale" );
IECore::InternedString g_shapeVolumeStepScaleAttributeName( "ai:shape:step_scale" );
IECore::InternedString g_shapeVolumeStepSizeAttributeName( "ai:shape:step_size" );
IECore::InternedString g_shapeVolumePaddingAttributeName( "ai:shape:volume_padding" );
IECore::InternedString g_volumeGridsAttributeName( "ai:volume:grids" );
IECore::InternedString g_velocityGridsAttributeName( "ai:volume:velocity_grids" );
IECore::InternedString g_velocityScaleAttributeName( "ai:volume:velocity_scale" );
IECore::InternedString g_velocityFPSAttributeName( "ai:volume:velocity_fps" );
IECore::InternedString g_velocityOutlierThresholdAttributeName( "ai:volume:velocity_outlier_threshold" );

IECore::InternedString g_transformTypeAttributeName( "ai:transform_type" );

IECore::InternedString g_polyMeshSubdivIterationsAttributeName( "ai:polymesh:subdiv_iterations" );
IECore::InternedString g_polyMeshSubdivAdaptiveErrorAttributeName( "ai:polymesh:subdiv_adaptive_error" );
IECore::InternedString g_polyMeshSubdivAdaptiveMetricAttributeName( "ai:polymesh:subdiv_adaptive_metric" );
IECore::InternedString g_polyMeshSubdivAdaptiveSpaceAttributeName( "ai:polymesh:subdiv_adaptive_space" );
IECore::InternedString g_polyMeshSubdivSmoothDerivsAttributeName( "ai:polymesh:subdiv_smooth_derivs" );
IECore::InternedString g_polyMeshSubdividePolygonsAttributeName( "ai:polymesh:subdivide_polygons" );
IECore::InternedString g_polyMeshSubdivUVSmoothingAttributeName( "ai:polymesh:subdiv_uv_smoothing" );

IECore::InternedString g_dispMapAttributeName( "ai:disp_map" );
IECore::InternedString g_dispHeightAttributeName( "ai:disp_height" );
IECore::InternedString g_dispPaddingAttributeName( "ai:disp_padding" );
IECore::InternedString g_dispZeroValueAttributeName( "ai:disp_zero_value" );
IECore::InternedString g_dispAutoBumpAttributeName( "ai:disp_autobump" );

IECore::InternedString g_curvesMinPixelWidthAttributeName( "ai:curves:min_pixel_width" );
IECore::InternedString g_curvesModeAttributeName( "ai:curves:mode" );
IECore::InternedString g_sssSetNameName( "ai:sss_setname" );

IECore::InternedString g_linkedLights( "linkedLights" );
IECore::InternedString g_lightFilterPrefix( "ai:lightFilter:" );

IECore::InternedString g_filteredLights( "filteredLights" );

class ArnoldAttributes : public IECoreScenePreview::Renderer::AttributesInterface
{

	public :

		ArnoldAttributes( const IECore::CompoundObject *attributes, ShaderCache *shaderCache, LightListCache *lightLinkCache, LightFilterConnections *lightFilterConnections )
			:	m_visibility( AI_RAY_ALL ), m_sidedness( AI_RAY_ALL ), m_shadingFlags( Default ), m_stepSize( 0.0f ), m_stepScale( 1.0f ), m_volumePadding( 0.0f ), m_polyMesh( attributes ), m_displacement( attributes, shaderCache ), m_curves( attributes ), m_volume( attributes ), m_lightListCache( lightLinkCache )
		{
			updateVisibility( g_cameraVisibilityAttributeName, AI_RAY_CAMERA, attributes );
			updateVisibility( g_shadowVisibilityAttributeName, AI_RAY_SHADOW, attributes );
			updateVisibility( g_diffuseReflectVisibilityAttributeName, AI_RAY_DIFFUSE_REFLECT, attributes );
			updateVisibility( g_specularReflectVisibilityAttributeName, AI_RAY_SPECULAR_REFLECT, attributes );
			updateVisibility( g_diffuseTransmitVisibilityAttributeName, AI_RAY_DIFFUSE_TRANSMIT, attributes );
			updateVisibility( g_specularTransmitVisibilityAttributeName, AI_RAY_SPECULAR_TRANSMIT, attributes );
			updateVisibility( g_volumeVisibilityAttributeName, AI_RAY_VOLUME, attributes );
			updateVisibility( g_subsurfaceVisibilityAttributeName, AI_RAY_SUBSURFACE, attributes );

			if( const IECore::BoolData *d = attribute<IECore::BoolData>( g_doubleSidedAttributeName, attributes ) )
			{
				m_sidedness = d->readable() ? AI_RAY_ALL : AI_RAY_UNDEFINED;
			}

			updateShadingFlag( g_arnoldReceiveShadowsAttributeName, ReceiveShadows, attributes );
			updateShadingFlag( g_arnoldSelfShadowsAttributeName, SelfShadows, attributes );
			updateShadingFlag( g_arnoldOpaqueAttributeName, Opaque, attributes );
			updateShadingFlag( g_arnoldMatteAttributeName, Matte, attributes );

			const IECoreScene::ShaderNetwork *surfaceShaderAttribute = attribute<IECoreScene::ShaderNetwork>( g_arnoldSurfaceShaderAttributeName, attributes );
			surfaceShaderAttribute = surfaceShaderAttribute ? surfaceShaderAttribute : attribute<IECoreScene::ShaderNetwork>( g_oslSurfaceShaderAttributeName, attributes );
			/// \todo Remove support for interpreting "osl:shader" as a surface shader assignment.
			surfaceShaderAttribute = surfaceShaderAttribute ? surfaceShaderAttribute : attribute<IECoreScene::ShaderNetwork>( g_oslShaderAttributeName, attributes );
			surfaceShaderAttribute = surfaceShaderAttribute ? surfaceShaderAttribute : attribute<IECoreScene::ShaderNetwork>( g_surfaceShaderAttributeName, attributes );
			if( surfaceShaderAttribute )
			{
				m_surfaceShader = shaderCache->get( surfaceShaderAttribute );
			}

			if( auto filterMapAttribute = attribute<IECoreScene::ShaderNetwork>( g_arnoldFilterMapAttributeName, attributes ) )
			{
				m_filterMap = shaderCache->get( filterMapAttribute );
			}
			if( auto uvRemapAttribute = attribute<IECoreScene::ShaderNetwork>( g_arnoldUVRemapAttributeName, attributes ) )
			{
				m_uvRemap = shaderCache->get( uvRemapAttribute );
			}

			m_lightShader = attribute<IECoreScene::ShaderNetwork>( g_arnoldLightShaderAttributeName, attributes );
			m_lightShader = m_lightShader ? m_lightShader : attribute<IECoreScene::ShaderNetwork>( g_lightShaderAttributeName, attributes );

			m_lightFilterShader = attribute<IECoreScene::ShaderNetwork>( g_arnoldLightFilterShaderAttributeName, attributes );

			m_traceSets = attribute<IECore::InternedStringVectorData>( g_setsAttributeName, attributes );
			m_transformType = attribute<IECore::StringData>( g_transformTypeAttributeName, attributes );
			m_stepSize = attributeValue<float>( g_shapeVolumeStepSizeAttributeName, attributes, 0.0f );
			m_stepScale = attributeValue<float>( g_shapeVolumeStepScaleAttributeName, attributes, 1.0f );
			m_volumePadding = attributeValue<float>( g_shapeVolumePaddingAttributeName, attributes, 0.0f );

			m_linkedLights = attribute<IECore::StringVectorData>( g_linkedLights, attributes );
			m_shadowGroup = attribute<IECore::StringVectorData>( g_shadowGroup, attributes );
			m_filteredLights = attribute<IECore::StringVectorData>( g_filteredLights, attributes );
			m_sssSetName = attribute<IECore::StringData>( g_sssSetNameName, attributes );

			for( IECore::CompoundObject::ObjectMap::const_iterator it = attributes->members().begin(), eIt = attributes->members().end(); it != eIt; ++it )
			{
				if( boost::starts_with( it->first.string(), "user:" ) )
				{
					if( const IECore::Data *data = IECore::runTimeCast<const IECore::Data>( it->second.get() ) )
					{
						m_user[it->first] = data;
					}
				}

				if( it->first.string() == g_arnoldLightFilterShaderAttributeName )
				{
					continue;
				}
				else if( boost::starts_with( it->first.string(), g_lightFilterPrefix.string() ) )
				{
					ArnoldShaderPtr filter = shaderCache->get( IECore::runTimeCast<const IECoreScene::ShaderNetwork>( it->second.get() ) );
					m_lightFilterShaders.push_back( filter );
				}
			}
		}

		// Some attributes affect the geometric properties of a node, which means they
		// go on the shape rather than the ginstance. These are problematic because they
		// must be taken into account when determining the hash for instancing, and
		// because they cannot be edited interactively. This method applies those
		// attributes, and is called from InstanceCache during geometry conversion.
		void applyGeometry( const IECore::Object *object, AtNode *node ) const
		{
			if( const IECoreScene::MeshPrimitive *mesh = IECore::runTimeCast<const IECoreScene::MeshPrimitive>( object ) )
			{
				m_polyMesh.apply( mesh, node );
				m_displacement.apply( node );
			}
			else if( IECore::runTimeCast<const IECoreScene::CurvesPrimitive>( object ) )
			{
				m_curves.apply( node );
			}
			else if( IECore::runTimeCast<const IECoreVDB::VDBObject>( object ) )
			{
				m_volume.apply( node );
			}
			else if( const IECoreScene::ExternalProcedural *procedural = IECore::runTimeCast<const IECoreScene::ExternalProcedural>( object ) )
			{
				if( procedural->getFileName() == "volume" )
				{
					m_volume.apply( node );
				}
			}

			float actualStepSize = m_stepSize * m_stepScale;

			if( actualStepSize != 0.0f && AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( node ), g_stepSizeArnoldString ) )
			{
				// Only apply step_size if it hasn't already been set to a non-zero
				// value by the geometry converter. This allows procedurals to carry
				// their step size as a parameter and have it trump the attribute value.
				// This is important for Gaffer nodes like ArnoldVDB, which carefully
				// calculate the correct step size and provide it via a parameter.
				if( AiNodeGetFlt( node, g_stepSizeArnoldString ) == 0.0f )
				{
					AiNodeSetFlt( node, g_stepSizeArnoldString, actualStepSize );
				}
			}

			if( m_volumePadding != 0.0f && AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( node ), g_volumePaddingArnoldString ) )
			{
				AiNodeSetFlt( node, g_volumePaddingArnoldString, m_volumePadding );
			}

		}

		// Generates a signature for the work done by applyGeometry.
		void hashGeometry( const IECore::Object *object, IECore::MurmurHash &h ) const
		{
			const IECore::TypeId objectType = object->typeId();
			bool meshInterpolationIsLinear = false;
			bool proceduralIsVolumetric = false;
			if( objectType == IECoreScene::MeshPrimitive::staticTypeId() )
			{
				meshInterpolationIsLinear = static_cast<const IECoreScene::MeshPrimitive *>( object )->interpolation() == "linear";
			}
			else if( objectType == IECoreScene::ExternalProcedural::staticTypeId() )
			{
				const IECoreScene::ExternalProcedural *procedural = static_cast<const IECoreScene::ExternalProcedural *>( object );
				if( procedural->getFileName() == "volume" )
				{
					proceduralIsVolumetric = true;
				}
			}
			hashGeometryInternal( objectType, meshInterpolationIsLinear, proceduralIsVolumetric, h );
		}

		// Returns true if the given geometry can be instanced, given the attributes that
		// will be applied in `applyGeometry()`.
		bool canInstanceGeometry( const IECore::Object *object ) const
		{
			if( !IECore::runTimeCast<const IECoreScene::VisibleRenderable>( object ) )
			{
				return false;
			}

			if( const IECoreScene::MeshPrimitive *mesh = IECore::runTimeCast<const IECoreScene::MeshPrimitive>( object ) )
			{
				if( mesh->interpolation() == "linear" )
				{
					return true;
				}
				else
				{
					// We shouldn't instance poly meshes with view dependent subdivision, because the subdivision
					// for the master mesh might be totally inappropriate for the position of the ginstances in frame.
					return m_polyMesh.subdivAdaptiveError == 0.0f || m_polyMesh.subdivAdaptiveSpace == g_objectArnoldString;
				}
			}
			else if( const IECoreScene::ExternalProcedural *procedural = IECore::runTimeCast<const IECoreScene::ExternalProcedural>( object ) )
			{
				// We don't instance "ass archive" procedurals, because Arnold
				// does automatic instancing of those itself, using its procedural
				// cache.
				return (
					!boost::ends_with( procedural->getFileName(), ".ass" ) &&
					!boost::ends_with( procedural->getFileName(), ".ass.gz" )
				);
			}

			return true;
		}

		// Most attributes (visibility, surface shader etc) are orthogonal to the
		// type of object to which they are applied. These are the good kind, because
		// they can be applied to ginstance nodes, making attribute edits easy. This
		// method applies those attributes, and is called from `Renderer::object()`
		// and `Renderer::attributes()`.
		//
		// The previousAttributes are passed so that we can check that the new
		// geometry attributes are compatible with those which were applied previously
		// (and which cannot be changed now). Returns true if all is well and false
		// if there is a clash (and the edit has therefore failed).
		bool apply( AtNode *node, const ArnoldAttributes *previousAttributes, bool applyLinkedLights ) const
		{

			// Check that we're not looking at an impossible request
			// to edit geometric attributes.

			if( previousAttributes )
			{
				const AtNode *geometry = node;
				if( AiNodeIs( node, g_ginstanceArnoldString ) )
				{
					geometry = static_cast<const AtNode *>( AiNodeGetPtr( node, g_nodeArnoldString ) );
				}

				IECore::TypeId objectType = IECore::InvalidTypeId;
				bool meshInterpolationIsLinear = false;
				bool proceduralIsVolumetric = false;
				if( AiNodeIs( geometry, g_polymeshArnoldString ) )
				{
					objectType = IECoreScene::MeshPrimitive::staticTypeId();
					meshInterpolationIsLinear = AiNodeGetStr( geometry, g_subdivTypeArnoldString ) != g_catclarkArnoldString;
				}
				else if( AiNodeIs( geometry, g_curvesArnoldString ) )
				{
					objectType = IECoreScene::CurvesPrimitive::staticTypeId();
				}
				else if( AiNodeIs( geometry, g_boxArnoldString ) )
				{
					objectType = IECoreScene::MeshPrimitive::staticTypeId();
				}
				else if( AiNodeIs( geometry, g_volumeArnoldString ) )
				{
					objectType = IECoreScene::ExternalProcedural::staticTypeId();
					proceduralIsVolumetric = true;
				}
				else if( AiNodeIs( geometry, g_sphereArnoldString ) )
				{
					objectType = IECoreScene::SpherePrimitive::staticTypeId();
				}

				IECore::MurmurHash previousGeometryHash;
				previousAttributes->hashGeometryInternal( objectType, meshInterpolationIsLinear, proceduralIsVolumetric, previousGeometryHash );

				IECore::MurmurHash currentGeometryHash;
				hashGeometryInternal( objectType, meshInterpolationIsLinear, proceduralIsVolumetric, currentGeometryHash );

				if( previousGeometryHash != currentGeometryHash )
				{
					return false;
				}
			}

			// Remove old user parameters we don't want any more.

			AtUserParamIterator *it= AiNodeGetUserParamIterator( node );
			while( !AiUserParamIteratorFinished( it ) )
			{
				const AtUserParamEntry *param = AiUserParamIteratorGetNext( it );
				const char *name = AiUserParamGetName( param );
				if( boost::starts_with( name, "user:" ) )
				{
					if( m_user.find( name ) == m_user.end() )
					{
						AiNodeResetParameter( node, name );
					}
				}
			}
			AiUserParamIteratorDestroy( it );

			// Add user parameters we do want.

			for( ArnoldAttributes::UserAttributes::const_iterator it = m_user.begin(), eIt = m_user.end(); it != eIt; ++it )
			{
				ParameterAlgo::setParameter( node, it->first.c_str(), it->second.get() );
			}

			// Add shape specific parameters.

			if( AiNodeEntryGetType( AiNodeGetNodeEntry( node ) ) == AI_NODE_SHAPE )
			{
				AiNodeSetByte( node, g_visibilityArnoldString, m_visibility );
				AiNodeSetByte( node, g_sidednessArnoldString, m_sidedness );

				if( m_transformType )
				{
					// \todo : Arnold quite explicitly discourages constructing AtStrings repeatedly,
					// but given the need to pass m_transformType around as a string for consistency
					// reasons, it seems like there's not much else we can do here.
					// If we start reusing ArnoldAttributes for multiple locations with identical attributes,
					// it could be worth caching this, or possibly in the future we could come up with
					// some way of cleanly exposing enum values as something other than strings.
					AiNodeSetStr( node, g_transformTypeArnoldString, AtString( m_transformType->readable().c_str() ) );
				}

				AiNodeSetBool( node, g_receiveShadowsArnoldString, m_shadingFlags & ArnoldAttributes::ReceiveShadows );
				AiNodeSetBool( node, g_selfShadowsArnoldString, m_shadingFlags & ArnoldAttributes::SelfShadows );
				AiNodeSetBool( node, g_opaqueArnoldString, m_shadingFlags & ArnoldAttributes::Opaque );
				AiNodeSetBool( node, g_matteArnoldString, m_shadingFlags & ArnoldAttributes::Matte );

				if( m_surfaceShader && m_surfaceShader->root() )
				{
					AiNodeSetPtr( node, g_shaderArnoldString, m_surfaceShader->root() );
				}
				else
				{
					AiNodeResetParameter( node, g_shaderArnoldString );
				}

				if( m_traceSets && m_traceSets->readable().size() )
				{
					const vector<IECore::InternedString> &v = m_traceSets->readable();
					AtArray *array = AiArrayAllocate( v.size(), 1, AI_TYPE_STRING );
					for( size_t i = 0, e = v.size(); i < e; ++i )
					{
						AiArraySetStr( array, i, v[i].c_str() );
					}
					AiNodeSetArray( node, g_traceSetsArnoldString, array );
				}
				else
				{
					// Arnold very unhelpfully treats `trace_sets == []` as meaning the object
					// is in every trace set. So we instead make `trace_sets == [ "__none__" ]`
					// to get the behaviour people expect.
					AiNodeSetArray( node, g_traceSetsArnoldString, AiArray( 1, 1, AI_TYPE_STRING, "__none__" ) );
				}

				if( m_sssSetName )
				{
					ParameterAlgo::setParameter( node, g_sssSetNameArnoldString, m_sssSetName.get() );
				}
				else
				{
					AiNodeResetParameter( node, g_sssSetNameArnoldString );
				}

				if( m_linkedLights && applyLinkedLights )
				{
					const std::vector<AtNode *> &linkedLightNodes = m_lightListCache->get( m_linkedLights.get() );

					AiNodeSetArray( node, g_lightGroupArnoldString, AiArrayConvert( linkedLightNodes.size(), 1, AI_TYPE_NODE, linkedLightNodes.data() ) );
					AiNodeSetBool( node, g_useLightGroupArnoldString, true );
				}
				else
				{
					AiNodeResetParameter( node, g_lightGroupArnoldString );
					AiNodeResetParameter( node, g_useLightGroupArnoldString );
				}

				if( m_shadowGroup && applyLinkedLights )
				{
					const std::vector<AtNode *> &linkedLightNodes = m_lightListCache->get( m_shadowGroup.get() );

					AiNodeSetArray( node, g_shadowGroupArnoldString, AiArrayConvert( linkedLightNodes.size(), 1, AI_TYPE_NODE, linkedLightNodes.data() ) );
					AiNodeSetBool( node, g_useShadowGroupArnoldString, true );
				}
				else
				{
					AiNodeResetParameter( node, g_shadowGroupArnoldString );
					AiNodeResetParameter( node, g_useShadowGroupArnoldString );
				}

			}

			// Add camera specific parameters.

			if( AiNodeEntryGetType( AiNodeGetNodeEntry( node ) ) == AI_NODE_CAMERA )
			{
				if( AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( node ), g_filterMapArnoldString ) )
				{
					if( m_filterMap && m_filterMap->root() )
					{
						AiNodeSetPtr( node, g_filterMapArnoldString, m_filterMap->root() );
					}
					else
					{
						AiNodeResetParameter( node, g_filterMapArnoldString );
					}
				}

				if( AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( node ), g_uvRemapArnoldString ) )
				{
					if( m_uvRemap && m_uvRemap->root() )
					{
						AiNodeLinkOutput( m_uvRemap->root(), "", node, g_uvRemapArnoldString );
					}
					else
					{
						AiNodeResetParameter( node, g_uvRemapArnoldString );
					}
				}
			}

			return true;
		}

		const IECoreScene::ShaderNetwork *lightShader() const
		{
			return m_lightShader.get();
		}

		/// Return the shader assigned to a world space light filter
		const IECoreScene::ShaderNetwork *lightFilterShader() const
		{
			return m_lightFilterShader.get();
		}

		const IECore::StringVectorData *filteredLights() const
		{
			return m_filteredLights.get();
		}

		/// Return the shaders for filters directly assigned to a light
		const std::vector<ArnoldShaderPtr>& lightFilterShaders() const
		{
			return m_lightFilterShaders;
		}

	private :

		struct PolyMesh
		{

			PolyMesh( const IECore::CompoundObject *attributes )
			{
				subdivIterations = attributeValue<int>( g_polyMeshSubdivIterationsAttributeName, attributes, 1 );
				subdivAdaptiveError = attributeValue<float>( g_polyMeshSubdivAdaptiveErrorAttributeName, attributes, 0.0f );

				const IECore::StringData *subdivAdaptiveMetricData = attribute<IECore::StringData>( g_polyMeshSubdivAdaptiveMetricAttributeName, attributes );
				if( subdivAdaptiveMetricData )
				{
					subdivAdaptiveMetric = AtString( subdivAdaptiveMetricData->readable().c_str() );
				}
				else
				{
					subdivAdaptiveMetric = g_autoArnoldString;
				}

				const IECore::StringData *subdivAdaptiveSpaceData = attribute<IECore::StringData>( g_polyMeshSubdivAdaptiveSpaceAttributeName, attributes );
				if( subdivAdaptiveSpaceData )
				{
					subdivAdaptiveSpace = AtString( subdivAdaptiveSpaceData->readable().c_str() );
				}
				else
				{
					subdivAdaptiveSpace = g_rasterArnoldString;
				}

				if( auto a = attribute<IECore::StringData>( g_polyMeshSubdivUVSmoothingAttributeName, attributes ) )
				{
					subdivUVSmoothing = AtString( a->readable().c_str() );
				}
				else
				{
					subdivUVSmoothing = g_pinCornersArnoldString;
				}

				subdividePolygons = attributeValue<bool>( g_polyMeshSubdividePolygonsAttributeName, attributes, false );
				subdivSmoothDerivs = attributeValue<bool>( g_polyMeshSubdivSmoothDerivsAttributeName, attributes, false );
			}

			int subdivIterations;
			float subdivAdaptiveError;
			AtString subdivAdaptiveMetric;
			AtString subdivAdaptiveSpace;
			AtString subdivUVSmoothing;
			bool subdividePolygons;
			bool subdivSmoothDerivs;

			void hash( bool meshInterpolationIsLinear, IECore::MurmurHash &h ) const
			{
				if( !meshInterpolationIsLinear || subdividePolygons )
				{
					h.append( subdivIterations );
					h.append( subdivAdaptiveError );
					h.append( subdivAdaptiveMetric.c_str() );
					h.append( subdivAdaptiveSpace.c_str() );
					h.append( subdivUVSmoothing.c_str() );
					h.append( subdivSmoothDerivs );
				}
			}

			void apply( const IECoreScene::MeshPrimitive *mesh, AtNode *node ) const
			{
				if( mesh->interpolation() != "linear" || subdividePolygons )
				{
					AiNodeSetByte( node, g_subdivIterationsArnoldString, subdivIterations );
					AiNodeSetFlt( node, g_subdivAdaptiveErrorArnoldString, subdivAdaptiveError );
					AiNodeSetStr( node, g_subdivAdaptiveMetricArnoldString, subdivAdaptiveMetric );
					AiNodeSetStr( node, g_subdivAdaptiveSpaceArnoldString, subdivAdaptiveSpace );
					AiNodeSetStr( node, g_subdivUVSmoothingArnoldString, subdivUVSmoothing );
					AiNodeSetBool( node, g_subdivSmoothDerivsArnoldString, subdivSmoothDerivs );
					if( mesh->interpolation() == "linear" )
					{
						AiNodeSetStr( node, g_subdivTypeArnoldString, g_linearArnoldString );
					}
				}
			}

		};

		struct Displacement
		{

			Displacement( const IECore::CompoundObject *attributes, ShaderCache *shaderCache )
			{
				if( const IECoreScene::ShaderNetwork *mapAttribute = attribute<IECoreScene::ShaderNetwork>( g_dispMapAttributeName, attributes ) )
				{
					map = shaderCache->get( mapAttribute );
				}
				height = attributeValue<float>( g_dispHeightAttributeName, attributes, 1.0f );
				padding = attributeValue<float>( g_dispPaddingAttributeName, attributes, 0.0f );
				zeroValue = attributeValue<float>( g_dispZeroValueAttributeName, attributes, 0.0f );
				autoBump = attributeValue<bool>( g_dispAutoBumpAttributeName, attributes, false );
			}

			ArnoldShaderPtr map;
			float height;
			float padding;
			float zeroValue;
			bool autoBump;

			void hash( IECore::MurmurHash &h ) const
			{
				if( map && map->root() )
				{
					h.append( AiNodeGetName( map->root() ) );
				}
				h.append( height );
				h.append( padding );
				h.append( zeroValue );
				h.append( autoBump );
			}

			void apply( AtNode *node ) const
			{
				if( map && map->root() )
				{
					AiNodeSetPtr( node, g_dispMapArnoldString, map->root() );
				}
				else
				{
					AiNodeResetParameter( node, g_dispMapArnoldString );
				}

				AiNodeSetFlt( node, g_dispHeightArnoldString, height );
				AiNodeSetFlt( node, g_dispPaddingArnoldString, padding );
				AiNodeSetFlt( node, g_dispZeroValueArnoldString, zeroValue );
				AiNodeSetBool( node, g_dispAutoBumpArnoldString, autoBump );
			}

		};

		struct Curves
		{

			Curves( const IECore::CompoundObject *attributes )
			{
				minPixelWidth = attributeValue<float>( g_curvesMinPixelWidthAttributeName, attributes, 0.0f );
				// Arnold actually has three modes - "ribbon", "oriented" and "thick".
				// The Cortex convention (inherited from RenderMan) is that curves without
				// normals ("N" primitive variable) are rendered as camera facing ribbons,
				// and those with normals are rendered as ribbons oriented by "N".
				// IECoreArnold::CurvesAlgo takes care of this part for us automatically, so all that
				// remains for us to do is to override the mode to "thick" if necessary to
				// expose Arnold's remaining functionality.
				//
				// The semantics for our "ai:curves:mode" attribute are therefore as follows :
				//
				//	  "ribbon" : Automatically choose `mode = "ribbon"` or `mode = "oriented"`
				//               according to the existence of "N".
				//    "thick"  : Render with `mode = "thick"`.
				thick = attributeValue<string>( g_curvesModeAttributeName, attributes, "ribbon" ) == "thick";
			}

			float minPixelWidth;
			bool thick;

			void hash( IECore::MurmurHash &h ) const
			{
				h.append( minPixelWidth );
				h.append( thick );
			}

			void apply( AtNode *node ) const
			{
				AiNodeSetFlt( node, g_minPixelWidthArnoldString, minPixelWidth );
				if( thick )
				{
					AiNodeSetStr( node, g_modeArnoldString, g_thickArnoldString );
				}
			}

		};

	struct Volume
	{
		Volume( const IECore::CompoundObject *attributes )
		{
			volumeGrids = attribute<IECore::StringVectorData>( g_volumeGridsAttributeName, attributes );
			velocityGrids = attribute<IECore::StringVectorData>( g_velocityGridsAttributeName, attributes );
			velocityScale = optionalAttribute<float>( g_velocityScaleAttributeName, attributes );
			velocityFPS = optionalAttribute<float>( g_velocityFPSAttributeName, attributes );
			velocityOutlierThreshold = optionalAttribute<float>( g_velocityOutlierThresholdAttributeName, attributes );
			stepSize = optionalAttribute<float> ( g_volumeStepSizeAttributeName, attributes );
			stepScale = optionalAttribute<float>( g_volumeStepScaleAttributeName, attributes );
		}

		IECore::ConstStringVectorDataPtr volumeGrids;
		IECore::ConstStringVectorDataPtr velocityGrids;
		boost::optional<float> velocityScale;
		boost::optional<float> velocityFPS;
		boost::optional<float> velocityOutlierThreshold;
		boost::optional<float> stepSize;
		boost::optional<float> stepScale;

		void hash( IECore::MurmurHash &h ) const
		{
			if( volumeGrids )
			{
				volumeGrids->hash( h );
			}

			if( velocityGrids )
			{
				velocityGrids->hash( h );
			}

			h.append( velocityScale.get_value_or( 1.0f ) );
			h.append( velocityFPS.get_value_or( 24.0f ) );
			h.append( velocityOutlierThreshold.get_value_or( 0.001f ) );
			h.append( stepSize.get_value_or( 0.0f ) );
			h.append( stepScale.get_value_or( 1.0f ) );
		}

		void apply( AtNode *node ) const
		{
			if( volumeGrids && volumeGrids->readable().size() )
			{
				AtArray *array = ParameterAlgo::dataToArray( volumeGrids.get(), AI_TYPE_STRING );
				AiNodeSetArray( node, g_volumeGridsArnoldString, array );
			}

			if( velocityGrids && velocityGrids->readable().size() )
			{
				AtArray *array = ParameterAlgo::dataToArray( velocityGrids.get(), AI_TYPE_STRING );
				AiNodeSetArray( node, g_velocityGridsArnoldString, array );
			}

			if( !velocityScale || velocityScale.get() > 0 )
			{
				AtNode *options = AiUniverseGetOptions();
				const AtNode *arnoldCamera = static_cast<const AtNode *>( AiNodeGetPtr( options, "camera" ) );

				if( arnoldCamera )
				{
					float shutterStart = AiNodeGetFlt( arnoldCamera, g_shutterStartArnoldString );
					float shutterEnd = AiNodeGetFlt( arnoldCamera, g_shutterEndArnoldString );

					// We're getting very lucky here:
					//  - Arnold has automatically set options.camera the first time we made a camera
					//  - All cameras output by Gaffer at present will have the same shutter,
					//    so it doesn't matter if we get it from the final render camera or not.
					AiNodeSetFlt( node, g_motionStartArnoldString, shutterStart );
					AiNodeSetFlt( node, g_motionEndArnoldString, shutterEnd );
				}
			}

			if( velocityScale )
			{
				AiNodeSetFlt( node, g_velocityScaleArnoldString, velocityScale.get() );
			}

			if( velocityFPS )
			{
				AiNodeSetFlt( node, g_velocityFPSArnoldString, velocityFPS.get() );
			}

			if( velocityOutlierThreshold )
			{
				AiNodeSetFlt( node, g_velocityOutlierThresholdArnoldString, velocityOutlierThreshold.get() );
			}

			if ( stepSize )
			{
				AiNodeSetFlt( node, g_stepSizeArnoldString, stepSize.get() * stepScale.get_value_or( 1.0f ) );
			}
			else if ( stepScale )
			{
				AiNodeSetFlt( node, g_stepScaleArnoldString, stepScale.get() );
			}
		}
	};

		enum ShadingFlags
		{
			ReceiveShadows = 1,
			SelfShadows = 2,
			Opaque = 4,
			Matte = 8,
			Default = ReceiveShadows | SelfShadows | Opaque,
			All = ReceiveShadows | SelfShadows | Opaque | Matte
		};

		template<typename T>
		static const T *attribute( const IECore::InternedString &name, const IECore::CompoundObject *attributes )
		{
			IECore::CompoundObject::ObjectMap::const_iterator it = attributes->members().find( name );
			if( it == attributes->members().end() )
			{
				return nullptr;
			}
			return reportedCast<const T>( it->second.get(), "attribute", name );
		}

		template<typename T>
		static T attributeValue( const IECore::InternedString &name, const IECore::CompoundObject *attributes, const T &defaultValue )
		{
			typedef IECore::TypedData<T> DataType;
			const DataType *data = attribute<DataType>( name, attributes );
			return data ? data->readable() : defaultValue;
		}

		template<typename T>
		static boost::optional<T> optionalAttribute( const IECore::InternedString &name, const IECore::CompoundObject *attributes )
		{
			typedef IECore::TypedData<T> DataType;
			const DataType *data = attribute<DataType>( name, attributes );
			return data ? data->readable() : boost::optional<T>();
		}

		void updateVisibility( const IECore::InternedString &name, unsigned char rayType, const IECore::CompoundObject *attributes )
		{
			if( const IECore::BoolData *d = attribute<IECore::BoolData>( name, attributes ) )
			{
				if( d->readable() )
				{
					m_visibility |= rayType;
				}
				else
				{
					m_visibility = m_visibility & ~rayType;
				}
			}
		}

		void updateShadingFlag( const IECore::InternedString &name, unsigned char flag, const IECore::CompoundObject *attributes )
		{
			if( const IECore::BoolData *d = attribute<IECore::BoolData>( name, attributes ) )
			{
				if( d->readable() )
				{
					m_shadingFlags |= flag;
				}
				else
				{
					m_shadingFlags = m_shadingFlags & ~flag;
				}
			}
		}

		void hashGeometryInternal( IECore::TypeId objectType, bool meshInterpolationIsLinear, bool proceduralIsVolumetric, IECore::MurmurHash &h ) const
		{
			switch( (int)objectType )
			{
				case IECoreScene::MeshPrimitiveTypeId :
					m_polyMesh.hash( meshInterpolationIsLinear, h );
					m_displacement.hash( h );
					h.append( m_stepSize );
					h.append( m_stepScale );
					h.append( m_volumePadding );
					break;
				case IECoreScene::CurvesPrimitiveTypeId :
					m_curves.hash( h );
					break;
				case IECoreScene::SpherePrimitiveTypeId :
					h.append( m_stepSize );
					h.append( m_stepScale );
					h.append( m_volumePadding );
					break;
				case IECoreScene::ExternalProceduralTypeId :
					if( proceduralIsVolumetric )
					{
						h.append( m_stepSize );
						h.append( m_stepScale );
						h.append( m_volumePadding );

						m_volume.hash( h );
					}
					break;
				case IECoreVDB::VDBObjectTypeId :
					h.append( m_volumePadding );

					m_volume.hash( h );
					break;
				default :
					// No geometry attributes for this type.
					break;
			}
		}

		unsigned char m_visibility;
		unsigned char m_sidedness;
		unsigned char m_shadingFlags;
		ArnoldShaderPtr m_surfaceShader;
		ArnoldShaderPtr m_filterMap;
		ArnoldShaderPtr m_uvRemap;
		IECoreScene::ConstShaderNetworkPtr m_lightShader;
		IECoreScene::ConstShaderNetworkPtr m_lightFilterShader;
		std::vector<ArnoldShaderPtr> m_lightFilterShaders;
		IECore::ConstInternedStringVectorDataPtr m_traceSets;
		IECore::ConstStringDataPtr m_transformType;
		float m_stepSize;
		float m_stepScale;
		float m_volumePadding;
		PolyMesh m_polyMesh;
		Displacement m_displacement;
		Curves m_curves;
		Volume m_volume;
		IECore::ConstStringVectorDataPtr m_linkedLights;
		IECore::ConstStringVectorDataPtr m_shadowGroup;
		IECore::ConstStringVectorDataPtr m_filteredLights;

		typedef boost::container::flat_map<IECore::InternedString, IECore::ConstDataPtr> UserAttributes;
		UserAttributes m_user;

		IECore::ConstStringDataPtr m_sssSetName;

		LightListCache *m_lightListCache;
};

IE_CORE_DECLAREPTR( ArnoldAttributes )

} // namespace

//////////////////////////////////////////////////////////////////////////
// InstanceCache
//////////////////////////////////////////////////////////////////////////

namespace
{

class Instance
{

	public :

		// Non-instanced
		Instance( const SharedAtNodePtr &node )
			:	m_node( node )
		{
		}

		// Instanced
		Instance( const SharedAtNodePtr &node, NodeDeleter nodeDeleter, const std::string &instanceName, const AtNode *parent )
			:	m_node( node )
		{
			if( node )
			{
				AiNodeSetByte( node.get(), g_visibilityArnoldString, 0 );
				m_ginstance = SharedAtNodePtr(
					AiNode( g_ginstanceArnoldString, AtString( instanceName.c_str() ), parent ),
					nodeDeleter
				);
				AiNodeSetPtr( m_ginstance.get(), g_nodeArnoldString, m_node.get() );
			}
		}

		AtNode *node()
		{
			return m_ginstance.get() ? m_ginstance.get() : m_node.get();
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			if( m_ginstance )
			{
				nodes.push_back( m_ginstance.get() );
			}
		}

	private :

		SharedAtNodePtr m_node;
		SharedAtNodePtr m_ginstance;

};

// Forward declaration
AtNode *convertProcedural( IECoreScenePreview::ConstProceduralPtr procedural, const std::string &nodeName, const AtNode *parentNode );

class InstanceCache : public IECore::RefCounted
{

	public :

		InstanceCache( NodeDeleter nodeDeleter, AtNode *parentNode )
			:	m_nodeDeleter( nodeDeleter ), m_parentNode( parentNode )
		{
		}

		// Can be called concurrently with other get() calls.
		Instance get( const IECore::Object *object, const IECoreScenePreview::Renderer::AttributesInterface *attributes, const std::string &nodeName )
		{
			const ArnoldAttributes *arnoldAttributes = static_cast<const ArnoldAttributes *>( attributes );

			if( !canInstance( object, arnoldAttributes ) )
			{
				return Instance( convert( object, arnoldAttributes, nodeName ) );
			}

			IECore::MurmurHash h = object->hash();
			arnoldAttributes->hashGeometry( object, h );

			Cache::accessor a;
			m_cache.insert( a, h );
			if( !a->second )
			{
				a->second = convert( object, arnoldAttributes, "instance:" + h.toString() );
			}

			return Instance( a->second, m_nodeDeleter, nodeName, m_parentNode );
		}

		Instance get( const std::vector<const IECore::Object *> &samples, const std::vector<float> &times, const IECoreScenePreview::Renderer::AttributesInterface *attributes, const std::string &nodeName )
		{
			const ArnoldAttributes *arnoldAttributes = static_cast<const ArnoldAttributes *>( attributes );

			if( !canInstance( samples.front(), arnoldAttributes ) )
			{
				return Instance( convert( samples, times, arnoldAttributes, nodeName ) );
			}

			IECore::MurmurHash h;
			for( std::vector<const IECore::Object *>::const_iterator it = samples.begin(), eIt = samples.end(); it != eIt; ++it )
			{
				(*it)->hash( h );
			}
			for( std::vector<float>::const_iterator it = times.begin(), eIt = times.end(); it != eIt; ++it )
			{
				h.append( *it );
			}
			arnoldAttributes->hashGeometry( samples.front(), h );

			Cache::accessor a;
			m_cache.insert( a, h );
			if( !a->second )
			{
				a->second = convert( samples, times, arnoldAttributes, "instance:" + h.toString() );
			}

			return Instance( a->second, m_nodeDeleter, nodeName, m_parentNode );
		}

		// Must not be called concurrently with anything.
		void clearUnused()
		{
			vector<IECore::MurmurHash> toErase;
			for( Cache::iterator it = m_cache.begin(), eIt = m_cache.end(); it != eIt; ++it )
			{
				if( it->second.unique() )
				{
					// Only one reference - this is ours, so
					// nothing outside of the cache is using the
					// node.
					toErase.push_back( it->first );
				}
			}
			for( vector<IECore::MurmurHash>::const_iterator it = toErase.begin(), eIt = toErase.end(); it != eIt; ++it )
			{
				m_cache.erase( *it );
			}
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			for( Cache::const_iterator it = m_cache.begin(), eIt = m_cache.end(); it != eIt; ++it )
			{
				if( it->second )
				{
					nodes.push_back( it->second.get() );
				}
			}
		}

	private :

		bool canInstance( const IECore::Object *object, const ArnoldAttributes *attributes ) const
		{
			if( IECore::runTimeCast<const IECoreScenePreview::Procedural>( object ) && m_nodeDeleter == AiNodeDestroy )
			{
				if( aiVersionLessThan( 5, 0, 1, 4 ) )
				{
					// Work around Arnold bug whereby deleting an instanced procedural
					// can lead to crashes. This unfortunately means that we don't get
					// to do instancing of procedurals during interactive renders, but
					// we can at least do it during batch renders.
					return false;
				}
			}
			return attributes->canInstanceGeometry( object );
		}

		SharedAtNodePtr convert( const IECore::Object *object, const ArnoldAttributes *attributes, const std::string &nodeName )
		{
			if( !object )
			{
				return SharedAtNodePtr();
			}

			AtNode *node = nullptr;
			if( const IECoreScenePreview::Procedural *procedural = IECore::runTimeCast<const IECoreScenePreview::Procedural>( object ) )
			{
				node = convertProcedural( procedural, nodeName, m_parentNode );
			}
			else
			{
				node = NodeAlgo::convert( object, nodeName, m_parentNode );
			}

			if( !node )
			{
				return SharedAtNodePtr();
			}

			attributes->applyGeometry( object, node );

			return SharedAtNodePtr( node, m_nodeDeleter );
		}

		SharedAtNodePtr convert( const std::vector<const IECore::Object *> &samples, const std::vector<float> &times, const ArnoldAttributes *attributes, const std::string &nodeName )
		{
			NodeAlgo::ensureUniformTimeSamples( times );
			AtNode *node = nullptr;
			if( const IECoreScenePreview::Procedural *procedural = IECore::runTimeCast<const IECoreScenePreview::Procedural>( samples.front() ) )
			{
				node = convertProcedural( procedural, nodeName, m_parentNode );
			}
			else
			{
				node = NodeAlgo::convert( samples, times[0], times[times.size() - 1], nodeName, m_parentNode );
			}

			if( !node )
			{
				return SharedAtNodePtr();
			}

			attributes->applyGeometry( samples.front(), node );

			return SharedAtNodePtr( node, m_nodeDeleter );

		}

		NodeDeleter m_nodeDeleter;
		AtNode *m_parentNode;

		typedef tbb::concurrent_hash_map<IECore::MurmurHash, SharedAtNodePtr> Cache;
		Cache m_cache;

};

IE_CORE_DECLAREPTR( InstanceCache )

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldObject
//////////////////////////////////////////////////////////////////////////

namespace
{

static IECore::InternedString g_surfaceAttributeName( "surface" );
static IECore::InternedString g_aiSurfaceAttributeName( "ai:surface" );

class ArnoldObject : public IECoreScenePreview::Renderer::ObjectInterface
{

	public :

		ArnoldObject( const Instance &instance, bool supportsLinkedLights = true )
			:	m_instance( instance ), m_attributes( nullptr ), m_supportsLinkedLights( supportsLinkedLights )
		{
		}

		void transform( const Imath::M44f &transform ) override
		{
			AtNode *node = m_instance.node();
			if( !node )
			{
				return;
			}
			applyTransform( node, transform );
		}

		void transform( const std::vector<Imath::M44f> &samples, const std::vector<float> &times ) override
		{
			AtNode *node = m_instance.node();
			if( !node )
			{
				return;
			}
			applyTransform( node, samples, times );
		}

		bool attributes( const IECoreScenePreview::Renderer::AttributesInterface *attributes ) override
		{
			AtNode *node = m_instance.node();
			if( !node )
			{
				return true;
			}

			const ArnoldAttributes *arnoldAttributes = static_cast<const ArnoldAttributes *>( attributes );
			if( arnoldAttributes->apply( node, m_attributes.get(), m_supportsLinkedLights ) )
			{
				m_attributes = arnoldAttributes;
				return true;
			}
			return false;
		}

		const Instance &instance() const
		{
			return m_instance;
		}

	protected :

		void applyTransform( AtNode *node, const Imath::M44f &transform, const AtString matrixParameterName = g_matrixArnoldString )
		{
			AiNodeSetMatrix( node, matrixParameterName, reinterpret_cast<const AtMatrix&>( transform.x ) );
		}

		void applyTransform( AtNode *node, const std::vector<Imath::M44f> &samples, const std::vector<float> &times, const AtString matrixParameterName = g_matrixArnoldString )
		{
			const size_t numSamples = samples.size();
			AtArray *matricesArray = AiArrayAllocate( 1, numSamples, AI_TYPE_MATRIX );
			for( size_t i = 0; i < numSamples; ++i )
			{
				AiArraySetMtx( matricesArray, i, reinterpret_cast<const AtMatrix&>( samples[i].x ) );
			}
			AiNodeSetArray( node, matrixParameterName, matricesArray );

			NodeAlgo::ensureUniformTimeSamples( times );
			AiNodeSetFlt( node, g_motionStartArnoldString, times[0] );
			AiNodeSetFlt( node, g_motionEndArnoldString, times[times.size() - 1] );

		}

		Instance m_instance;
		// We keep a reference to the currently applied attributes
		// for a couple of reasons :
		//
		//  - We need to keep the displacement and surface shaders
		//    alive for as long as they are referenced by m_instance.
		//  - We can use the previously applied attributes to determine
		//    if an incoming attribute edit is impossible because it
		//    would affect the instance itself, and return failure from
		//    `attributes()`.
		ConstArnoldAttributesPtr m_attributes;

	private :

		// Derived classes are allowed to reject having lights linked to them
		bool m_supportsLinkedLights;

};

IE_CORE_FORWARDDECLARE( ArnoldObject )

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldLightFilter
//////////////////////////////////////////////////////////////////////////

namespace
{

/// The LightFilterConnections can be used to record changes regarding connections
/// between world space light filters and lights and to communicate them to Arnold.
class LightFilterConnections : public IECore::RefCounted
{

public :

	LightFilterConnections( bool ownConnections )
		: m_ownConnections( ownConnections )
	{
	}

	// Record changes. These can be called concurrently.
	void registerLight( const std::string &lightName, ArnoldLight *light );
	void deregisterLight( const std::string &lightName );
	void registerLightFilter( const IECore::StringVectorData *lightNames, ArnoldLightFilter *lightFilter );
	void deregisterLightFilter( const IECore::StringVectorData *lightNames, ArnoldLightFilter *lightFilter );

	// Communicate with Arnold. This needs to get called non-concurrently before rendering.
	void update();

private:

	// Mapping between many lights and many filters to avoid N*M growth in
	// memory consumption for N lights and M filters. Every filter is put in a
	// group that collects all filters that affect the same set of lights. Each
	// light is then linked to all groups that affect the particular light.
	using FilterGroup = std::unordered_set<ArnoldLightFilter*>;
	using FilterGroupPtr = std::shared_ptr<FilterGroup>;
	using FilterGroups = tbb::concurrent_hash_map<const IECore::StringVectorData*, FilterGroupPtr>;

	struct Filters
	{
		ArnoldLight *light;
		std::unordered_set<FilterGroup*> lightFilterGroups;
		bool dirty;
	};

	using ConnectionsMap = tbb::concurrent_hash_map<const std::string, Filters>;
	ConnectionsMap m_connections;

	FilterGroups m_filterGroups;

	bool m_ownConnections;
	tbb::concurrent_vector<ArnoldObjectPtr> m_arnoldObjects;
};

IE_CORE_DECLAREPTR( LightFilterConnections )


class ArnoldLightFilter : public ArnoldObject
{

	public :

		ArnoldLightFilter( const std::string &name, const Instance &instance, NodeDeleter nodeDeleter, const AtNode *parentNode, LightFilterConnections *lightFilterConnections )
			:	ArnoldObject( instance, /* supportsLinkedLights */ false ), m_name( name ), m_nodeDeleter( nodeDeleter ), m_parentNode( parentNode ), m_connections( lightFilterConnections )
		{
		}

		~ArnoldLightFilter() override
		{
			const IECore::StringVectorData *filteredLightsData = m_attributes->filteredLights();
			m_connections->deregisterLightFilter( filteredLightsData, this );
		}

		void transform( const Imath::M44f &transform ) override
		{
			ArnoldObject::transform( transform );
			m_transformMatrices.clear();
			m_transformTimes.clear();
			m_transformMatrices.push_back( transform );
			applyLightFilterTransform();
		}

		void transform( const std::vector<Imath::M44f> &samples, const std::vector<float> &times ) override
		{
			ArnoldObject::transform( samples, times );
			m_transformMatrices = samples;
			m_transformTimes = times;
			applyLightFilterTransform();
		}

		bool attributes( const IECoreScenePreview::Renderer::AttributesInterface *attributes ) override
		{
			if( !ArnoldObject::attributes( attributes ) )
			{
				return false;
			}

			if( m_attributes )
			{
				// We have registered this light filter before, - undo that.
				const IECore::StringVectorData *previouslyFilteredLightsData = m_attributes->filteredLights();
				if( previouslyFilteredLightsData )
				{
					m_connections->deregisterLightFilter( previouslyFilteredLightsData, this );
				}
			}

			const ArnoldAttributes *arnoldAttributes = static_cast<const ArnoldAttributes *>( attributes );
			m_attributes = arnoldAttributes;

			// Update light filter shader if it is actually used to filter lights.

			const IECore::StringVectorData *filteredLightsData = arnoldAttributes->filteredLights();

			// Reset light filter until we know that we have all necessary data
			m_lightFilterShader = nullptr;

			if( !filteredLightsData || !arnoldAttributes->lightFilterShader() )
			{
				return true;
			}

			m_lightFilterShader = new ArnoldShader( arnoldAttributes->lightFilterShader(), m_nodeDeleter, "lightFilter:" + m_name + ":", m_parentNode );

			// Make sure light filter is registered so lights can use it
			m_connections->registerLightFilter( filteredLightsData, this );

			// Simplify name for the root shader, for ease of reading of ass files.
			const std::string name = "lightFilter:" + m_name;
			AiNodeSetStr( m_lightFilterShader->root(), g_nameArnoldString, AtString( name.c_str() ) );

			applyLightFilterTransform();

			return true;
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			if( m_lightFilterShader )
			{
				m_lightFilterShader->nodesCreated( nodes );
			}
		}

		ArnoldShader *lightFilterShader() const
		{
			return m_lightFilterShader.get();
		}

	private :

		void applyLightFilterTransform()
		{
			if( !m_lightFilterShader || m_transformMatrices.empty() )
			{
				return;
			}
			AtNode *root = m_lightFilterShader->root();
			if( m_transformTimes.empty() )
			{
				assert( m_transformMatrices.size() == 1 );
				applyTransform( root, m_transformMatrices[0], g_geometryMatrixArnoldString );
			}
			else
			{
				applyTransform( root, m_transformMatrices, m_transformTimes, g_geometryMatrixArnoldString );
			}
		}

		std::string m_name;
		vector<Imath::M44f> m_transformMatrices;
		vector<float> m_transformTimes;
		NodeDeleter m_nodeDeleter;
		const AtNode *m_parentNode;
		ArnoldShaderPtr m_lightFilterShader;
		LightFilterConnections *m_connections;
};

IE_CORE_DECLAREPTR( ArnoldLightFilter )

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldLight
//////////////////////////////////////////////////////////////////////////

namespace
{

class ArnoldLight : public ArnoldObject
{

	public :

		ArnoldLight( const std::string &name, const Instance &instance, NodeDeleter nodeDeleter, const AtNode *parentNode, LightFilterConnections *lightFilterConnections )
			:	ArnoldObject( instance, /* supportsLinkedLights */ false ), m_name( name ), m_nodeDeleter( nodeDeleter ), m_parentNode( parentNode ), m_connections( lightFilterConnections )
		{
			// Explicitly opted out of having lights linked to us, for two reasons :
			//
			// - It doesn't make much sense, because we're a light ourself.
			// - We can only apply light linking correctly once all lights have
			//   been output, otherwise LightListCache will be outputting partial
			//   lists. We have no idea if more lights will be output after this
			//   one.
			//
			/// \todo There is an argument for dealing with this in `GafferScene::RendererAlgo`
			/// instead. Reconsider when adding light linking to other renderer backends.
		}

		~ArnoldLight() override
		{
			m_connections->deregisterLight( m_name );
		}

		void transform( const Imath::M44f &transform ) override
		{
			ArnoldObject::transform( transform );
			m_transformMatrices.clear();
			m_transformTimes.clear();
			m_transformMatrices.push_back( transform );
			applyLightTransform();
		}

		void transform( const std::vector<Imath::M44f> &samples, const std::vector<float> &times ) override
		{
			ArnoldObject::transform( samples, times );
			m_transformMatrices = samples;
			m_transformTimes = times;
			applyLightTransform();
		}

		bool attributes( const IECoreScenePreview::Renderer::AttributesInterface *attributes ) override
		{
			if( !ArnoldObject::attributes( attributes ) )
			{
				return false;
			}

			const ArnoldAttributes *arnoldAttributes = static_cast<const ArnoldAttributes *>( attributes );
			m_attributes = arnoldAttributes;

			// Update light shader.

			// Delete current light shader, destroying all AtNodes it owns. It
			// is crucial that we do this _before_ constructing a new
			// ArnoldShader (and therefore AtNodes) below, because we are
			// relying on a specific behaviour of the Arnold node allocator.
			// When we destroy the light node, Arnold does not remove it from
			// any of the `light_group` arrays we have assigned to geometry,
			// meaning they will contain a dangling pointer. If we destroy the
			// old AtNode first, we get lucky, and Arnold will allocate the new
			// one at the _exact same address_ as the old one, keeping our
			// arrays valid. We have been accidentally relying on this behaviour
			// for some time, and for now continue to rely on it in lieu of a
			// more complex fix which might involve a `LightLinkManager` that is
			// able to track and patch up any affected light links. Because of
			// the extra bookkeeping involved in such an approach, we would want
			// to keep its use to a minimum. We could achieve that for the
			// common case by editing the light node's parameters in place, only
			// creating a new light node when the type has changed.
			m_lightShader = nullptr;

			if( !arnoldAttributes->lightShader() )
			{
				return true;
			}

			m_lightShader = new ArnoldShader( arnoldAttributes->lightShader(), m_nodeDeleter, "light:" + m_name + ":", m_parentNode );

			// Simplify name for the root shader, for ease of reading of ass files.
			const std::string name = "light:" + m_name;
			AiNodeSetStr( m_lightShader->root(), g_nameArnoldString, AtString( name.c_str() ) );

			// Deal with mesh lights.

			if( AiNodeIs( m_lightShader->root(), g_meshLightArnoldString ) )
			{
				if( m_instance.node() )
				{
					AiNodeSetPtr( m_lightShader->root(), g_meshArnoldString, m_instance.node() );
				}
				else
				{
					// Don't output mesh lights from locations with no object
					m_lightShader = nullptr;
					return true;
				}
			}

			// Deal with light filter connections

			// We re-register the light here because the light shader has been
			// replaced above. Without re-registering we wouldn't get a chance
			// to set the connections on that new shader in our `updateFilters`
			// method.
			m_connections->registerLight( m_name, this );

			applyLightTransform();

			return true;
		}

		void nodesCreated( vector<AtNode *> &nodes ) const
		{
			if( m_lightShader )
			{
				m_lightShader->nodesCreated( nodes );
			}
		}

		void updateFilters( std::vector<ArnoldLightFilter*> &lightFilters )
		{
			if( !m_lightShader )
			{
				return;
			}

			// In the following we're combining the world space light filters
			// with the ones that are assigned to the lights directly and live
			// in light space

			const std::vector<ArnoldShaderPtr> &lightFilterShaders = m_attributes->lightFilterShaders();

			size_t numShaders = lightFilters.size() + lightFilterShaders.size();
			AtArray *linkedFilterNodes = AiArrayAllocate( numShaders, 1, AI_TYPE_NODE );

			int idx = 0;
			for( const ArnoldLightFilter *lightFilter : lightFilters )
			{
				AiArraySetPtr( linkedFilterNodes, idx++, lightFilter->lightFilterShader()->root() );
			}

			for( const ArnoldShaderPtr &filter : lightFilterShaders )
			{
				AiArraySetPtr( linkedFilterNodes, idx++, filter->root() );
			}

			AiNodeSetArray( m_lightShader->root(), g_filtersArnoldString, linkedFilterNodes );
		}

	private :

		void applyLightTransform()
		{
			if( !m_lightShader || m_transformMatrices.empty() )
			{
				return;
			}
			AtNode *root = m_lightShader->root();
			if( m_transformTimes.empty() )
			{
				assert( m_transformMatrices.size() == 1 );
				applyTransform( root, m_transformMatrices[0] );
			}
			else
			{
				applyTransform( root, m_transformMatrices, m_transformTimes );
			}
		}

		// Because the AtNode for the light arrives via attributes(),
		// we need to store the transform and name ourselves so we have
		// them later when we need them.
		std::string m_name;
		vector<Imath::M44f> m_transformMatrices;
		vector<float> m_transformTimes;
		NodeDeleter m_nodeDeleter;
		const AtNode *m_parentNode;
		ArnoldShaderPtr m_lightShader;
		LightFilterConnections *m_connections;

};

IE_CORE_DECLAREPTR( ArnoldLight )

} // namespace


namespace
{

void LightFilterConnections::registerLight( const std::string &lightName, ArnoldLight *light )
{
	ConnectionsMap::accessor a;
	m_connections.insert( a, lightName );

	a->second.light = light;
	a->second.dirty = true;

	if( m_ownConnections )
	{
		m_arnoldObjects.emplace_back( light );
	}
}

void LightFilterConnections::deregisterLight( const std::string &lightName )
{

	ConnectionsMap::accessor a;
	if( m_connections.find( a, lightName ) )
	{
		a->second.light = nullptr;
		a->second.dirty = true;
	}
	else
	{
		IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", boost::str( boost::format( "Can not deregister light filter connections for non-existing light \"%s\"" ) % lightName.c_str() ) );
	}
}

void LightFilterConnections::registerLightFilter( const IECore::StringVectorData *lightNames, ArnoldLightFilter *lightFilter )
{
	// Add filter to group of filters stored for the given lights
	bool newFilterGroup( false );
	FilterGroup *filterGroup;
	{
		FilterGroups::accessor fga;
		m_filterGroups.insert( fga, lightNames );

		if( !fga->second )
		{
			fga->second = std::make_shared<FilterGroup>();
			newFilterGroup = true;
		}

		filterGroup = fga->second.get();
		filterGroup->insert( lightFilter );
	}

	// \todo: We're currently locking on the light and make other threads wait
	// although they could handle other lights already?
	for( const std::string &lightName : lightNames->readable() )
	{
		ConnectionsMap::accessor a;
		if( !m_connections.find( a, lightName ) )
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", boost::str( boost::format( "Can not register light filter connection for non-existing light \"%s\"" ) % lightName.c_str() ) );
		}

		if( newFilterGroup )
		{
			a->second.lightFilterGroups.insert( filterGroup );
		}

		// Even if we knew about the light filter already we need to dirty the light
		// as the filter itself might have been updated.
		a->second.dirty = true;
	}

	if( m_ownConnections )
	{
		m_arnoldObjects.emplace_back( lightFilter );
	}
}

void LightFilterConnections::deregisterLightFilter( const IECore::StringVectorData *lightNames, ArnoldLightFilter *lightFilter )
{
	bool filterErased( false );
	bool groupEmptied( false );
	FilterGroup *filterGroup;
	{
		FilterGroups::accessor fga;
		if( m_filterGroups.find( fga, lightNames ) )
		{
			filterGroup = fga->second.get();
			filterErased = filterGroup->erase( lightFilter );
			groupEmptied = filterGroup->empty();
		}
	}

	if( !filterErased )
	{
		return;
	}

	for( const std::string &lightName : lightNames->readable() )
	{
		ConnectionsMap::accessor a;
		if( m_connections.find( a, lightName ) )
		{
			if( groupEmptied )
			{
				a->second.lightFilterGroups.erase( filterGroup );
			}
			a->second.dirty = true;
		}
	}

	if( groupEmptied )
	{
		m_filterGroups.erase( lightNames );
	}
}

// Can not be called concurrently.
void LightFilterConnections::update()
{
	tbb::concurrent_vector<std::string> deregistered;

	parallel_for(
		m_connections.range(),
		[this, &deregistered]( ConnectionsMap::range_type &range )
		{
			for( auto it = range.begin(); it != range.end(); ++it )
			{
				if( !it->second.light )
				{
					deregistered.push_back( it->first );
					continue;
				}

				if( !it->second.dirty )
				{
					continue;
				}

				std::vector<ArnoldLightFilter*> allFilters;
				for( FilterGroup *filterGroup : it->second.lightFilterGroups )
				{
					allFilters.insert( allFilters.end(), filterGroup->begin(), filterGroup->end() );
				}

				it->second.light->updateFilters( allFilters );
				it->second.dirty = false;
			}
		},
		tbb::auto_partitioner()
	);

	if( m_ownConnections )
	{
		m_arnoldObjects.clear();
		return;
	}

	for( const std::string &lightName : deregistered )
	{
		m_connections.erase( lightName );
	}
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// Procedurals
//////////////////////////////////////////////////////////////////////////

namespace
{

class ProceduralRenderer final : public ArnoldRendererBase
{

	public :

		// We use a null node deleter because Arnold will automatically
		// destroy all nodes belonging to the procedural when the procedural
		// itself is destroyed.
		/// \todo The base class currently makes a new shader cache
		/// and a new instance cache. Can we share with the parent
		/// renderer instead?
		ProceduralRenderer( AtNode *procedural )
			:	ArnoldRendererBase( nullNodeDeleter, LightFilterConnectionsPtr( new LightFilterConnections( /* ownConnections = */ false ) ), procedural )
		{
		}

		void option( const IECore::InternedString &name, const IECore::Object *value ) override
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", "Procedurals can not call option()" );
		}

		void output( const IECore::InternedString &name, const IECoreScene::Output *output ) override
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", "Procedurals can not call output()" );
		}

		ObjectInterfacePtr camera( const std::string &name, const IECoreScene::Camera *camera, const AttributesInterface *attributes ) override
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", "Procedurals can not call camera()" );
			return nullptr;
		}

		ObjectInterfacePtr light( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override
		{
			ArnoldLightPtr result = static_pointer_cast<ArnoldLight>(
				ArnoldRendererBase::light( name, object, attributes )
			);

			NodesCreatedMutex::scoped_lock lock( m_nodesCreatedMutex );
			result->instance().nodesCreated( m_nodesCreated );
			result->nodesCreated( m_nodesCreated );
			return result;
		}

		ObjectInterfacePtr lightFilter( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override
		{
			ArnoldLightFilterPtr result = static_pointer_cast<ArnoldLightFilter>(
				ArnoldRendererBase::lightFilter( name, object, attributes )
			);

			NodesCreatedMutex::scoped_lock lock( m_nodesCreatedMutex );
			result->instance().nodesCreated( m_nodesCreated );
			result->nodesCreated( m_nodesCreated );
			return result;
		}

		Renderer::ObjectInterfacePtr object( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes ) override
		{
			ArnoldObjectPtr result = static_pointer_cast<ArnoldObject>(
				ArnoldRendererBase::object( name, object, attributes )
			);

			NodesCreatedMutex::scoped_lock lock( m_nodesCreatedMutex );
			result->instance().nodesCreated( m_nodesCreated );
			return result;
		}

		ObjectInterfacePtr object( const std::string &name, const std::vector<const IECore::Object *> &samples, const std::vector<float> &times, const AttributesInterface *attributes ) override
		{
			ArnoldObjectPtr result = static_pointer_cast<ArnoldObject>(
				ArnoldRendererBase::object( name, samples, times, attributes )
			);

			NodesCreatedMutex::scoped_lock lock( m_nodesCreatedMutex );
			result->instance().nodesCreated( m_nodesCreated );
			return result;
		}

		void render() override
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", "Procedurals can not call render()" );
		}

		void pause() override
		{
			IECore::msg( IECore::Msg::Warning, "ArnoldRenderer", "Procedurals can not call pause()" );
		}

		void nodesCreated( vector<AtNode *> &nodes )
		{
			nodes.insert( nodes.begin(), m_nodesCreated.begin(), m_nodesCreated.end() );
			m_instanceCache->nodesCreated( nodes );
			m_shaderCache->nodesCreated( nodes );
		}

	private :

		typedef tbb::spin_mutex NodesCreatedMutex;
		NodesCreatedMutex m_nodesCreatedMutex;
		vector<AtNode *> m_nodesCreated;

};

IE_CORE_DECLAREPTR( ProceduralRenderer )

struct ProceduralData : boost::noncopyable
{
	vector<AtNode *> nodesCreated;
};

int procInit( AtNode *node, void **userPtr )
{
	ProceduralData *data = (ProceduralData *)( AiNodeGetPtr( node, g_userPtrArnoldString ) );
	*userPtr = data;
	return 1;
}

int procCleanup( const AtNode *node, void *userPtr )
{
	const ProceduralData *data = (ProceduralData *)( userPtr );
	delete data;
	return 1;
}

int procNumNodes( const AtNode *node, void *userPtr )
{
	const ProceduralData *data = (ProceduralData *)( userPtr );
	return data->nodesCreated.size();
}

AtNode *procGetNode( const AtNode *node, void *userPtr, int i )
{
	const ProceduralData *data = (ProceduralData *)( userPtr );
	return data->nodesCreated[i];
}

int procFunc( AtProceduralNodeMethods *methods )
{
	methods->Init = procInit;
	methods->Cleanup = procCleanup;
	methods->NumNodes = procNumNodes;
	methods->GetNode = procGetNode;
	return 1;
}

AtNode *convertProcedural( IECoreScenePreview::ConstProceduralPtr procedural, const std::string &nodeName, const AtNode *parentNode )
{
	AtNode *node = AiNode( g_proceduralArnoldString, AtString( nodeName.c_str() ), parentNode );

	AiNodeSetPtr( node, g_funcPtrArnoldString, (void *)procFunc );

	ProceduralRendererPtr renderer = new ProceduralRenderer( node );
	procedural->render( renderer.get() );

	ProceduralData *data = new ProceduralData;
	renderer->nodesCreated( data->nodesCreated );
	AiNodeSetPtr( node, g_userPtrArnoldString, data );

	return node;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// InteractiveRenderController
//////////////////////////////////////////////////////////////////////////

namespace
{

class InteractiveRenderController
{

	public :

		InteractiveRenderController()
		{
			m_rendering = false;
		}

		void setRendering( bool rendering )
		{
			if( rendering == m_rendering )
			{
				return;
			}

			m_rendering = rendering;

			if( rendering )
			{
				std::thread thread( boost::bind( &InteractiveRenderController::performInteractiveRender, this ) );
				m_thread.swap( thread );
			}
			else
			{
				if( AiRendering() )
				{
					AiRenderInterrupt();
				}
				m_thread.join();
			}
		}

		bool getRendering() const
		{
			return m_rendering;
		}

	private :

		// Called in a background thread to control a
		// progressive interactive render.
		void performInteractiveRender()
		{
			AtNode *options = AiUniverseGetOptions();
			const int finalAASamples = AiNodeGetInt( options, g_aaSamplesArnoldString );
			const int startAASamples = min( -5, finalAASamples );

			for( int aaSamples = startAASamples; aaSamples <= finalAASamples; ++aaSamples )
			{
				if( aaSamples == 0 || ( aaSamples > 1 && aaSamples != finalAASamples ) )
				{
					// 0 AA_samples is meaningless, and we want to jump straight
					// from 1 AA_sample to the final sampling quality.
					continue;
				}

				AiNodeSetInt( options, g_aaSamplesArnoldString, aaSamples );
				if( !m_rendering || AiRender( AI_RENDER_MODE_CAMERA ) != AI_SUCCESS )
				{
					// Render cancelled on main thread.
					break;
				}
			}

			// Restore the setting we've been monkeying with.
			AiNodeSetInt( options, g_aaSamplesArnoldString, finalAASamples );
		}

		std::thread m_thread;
		tbb::atomic<bool> m_rendering;

};

} // namespace

//////////////////////////////////////////////////////////////////////////
// Globals
//////////////////////////////////////////////////////////////////////////

namespace
{

/// \todo Should these be defined in the Renderer base class?
/// Or maybe be in a utility header somewhere?
IECore::InternedString g_frameOptionName( "frame" );
IECore::InternedString g_cameraOptionName( "camera" );

IECore::InternedString g_logFileNameOptionName( "ai:log:filename" );
IECore::InternedString g_logMaxWarningsOptionName( "ai:log:max_warnings" );
IECore::InternedString g_statisticsFileNameOptionName( "ai:statisticsFileName" );
IECore::InternedString g_pluginSearchPathOptionName( "ai:plugin_searchpath" );
IECore::InternedString g_aaSeedOptionName( "ai:AA_seed" );
IECore::InternedString g_sampleMotionOptionName( "sampleMotion" );
IECore::InternedString g_atmosphereOptionName( "ai:atmosphere" );
IECore::InternedString g_backgroundOptionName( "ai:background" );

std::string g_logFlagsOptionPrefix( "ai:log:" );
std::string g_consoleFlagsOptionPrefix( "ai:console:" );

const int g_logFlagsDefault = AI_LOG_ALL;
const int g_consoleFlagsDefault = AI_LOG_WARNINGS | AI_LOG_ERRORS | AI_LOG_TIMESTAMP | AI_LOG_BACKTRACE | AI_LOG_MEMORY | AI_LOG_COLOR;

class ArnoldGlobals
{

	public :

		ArnoldGlobals( IECoreScenePreview::Renderer::RenderType renderType, const std::string &fileName, ShaderCache *shaderCache )
			:	m_renderType( renderType ),
				m_universeBlock( /* writable = */ true ),
				m_logFileFlags( g_logFlagsDefault ),
				m_consoleFlags( g_consoleFlagsDefault ),
				m_shaderCache( shaderCache ),
				m_assFileName( fileName )
		{
			AiMsgSetLogFileFlags( m_logFileFlags );
			AiMsgSetConsoleFlags( m_consoleFlags );
			// Get OSL shaders onto the shader searchpath.
			option( g_pluginSearchPathOptionName, new IECore::StringData( "" ) );
		}

		void option( const IECore::InternedString &name, const IECore::Object *value )
		{
			AtNode *options = AiUniverseGetOptions();
			if( name == g_frameOptionName )
			{
				if( value == nullptr )
				{
					m_frame = boost::none;
				}
				else if( const IECore::IntData *d = reportedCast<const IECore::IntData>( value, "option", name ) )
				{
					m_frame = d->readable();
				}
				return;
			}
			else if( name == g_cameraOptionName )
			{
				if( value == nullptr )
				{
					m_cameraName = "";
				}
				else if( const IECore::StringData *d = reportedCast<const IECore::StringData>( value, "option", name ) )
				{
					m_cameraName = d->readable();

				}
				return;
			}
			else if( name == g_logFileNameOptionName )
			{
				if( value == nullptr )
				{
					AiMsgSetLogFileName( "" );
				}
				else if( const IECore::StringData *d = reportedCast<const IECore::StringData>( value, "option", name ) )
				{
					if( !d->readable().empty() )
					{
						try
						{
							boost::filesystem::path path( d->readable() );
							path.remove_filename();
							boost::filesystem::create_directories( path );
						}
						catch( const std::exception &e )
						{
							IECore::msg( IECore::Msg::Error, "ArnoldRenderer::option()", e.what() );
						}
					}
					AiMsgSetLogFileName( d->readable().c_str() );

				}
				return;
			}
			else if( name == g_statisticsFileNameOptionName )
			{
				AiStatsSetMode( AI_STATS_MODE_OVERWRITE );

				if( value == nullptr )
				{
					AiStatsSetFileName( "" );
				}
				else if( const IECore::StringData *d = reportedCast<const IECore::StringData>( value, "option", name ) )
				{
					if( !d->readable().empty() )
					{
						try
						{
							boost::filesystem::path path( d->readable() );
							path.remove_filename();
							boost::filesystem::create_directories( path );
						}
						catch( const std::exception &e )
						{
							IECore::msg( IECore::Msg::Error, "ArnoldRenderer::option()", e.what() );
						}
					}
					AiStatsSetFileName( d->readable().c_str() );

				}
				return;
			}
			else if( name == g_logMaxWarningsOptionName )
			{
				if( value == nullptr )
				{
					AiMsgSetMaxWarnings( 100 );
				}
				else if( const IECore::IntData *d = reportedCast<const IECore::IntData>( value, "option", name ) )
				{
					AiMsgSetMaxWarnings( d->readable() );
				}
				return;
			}
			else if( boost::starts_with( name.c_str(), g_logFlagsOptionPrefix ) )
			{
				if( updateLogFlags( name.string().substr( g_logFlagsOptionPrefix.size() ), IECore::runTimeCast<const IECore::Data>( value ), /* console = */ false ) )
				{
					return;
				}
			}
			else if( boost::starts_with( name.c_str(), g_consoleFlagsOptionPrefix ) )
			{
				if( updateLogFlags( name.string().substr( g_consoleFlagsOptionPrefix.size() ), IECore::runTimeCast<const IECore::Data>( value ), /* console = */ true ) )
				{
					return;
				}
			}
			else if( name == g_aaSeedOptionName )
			{
				if( value == nullptr )
				{
					m_aaSeed = boost::none;
				}
				else if( const IECore::IntData *d = reportedCast<const IECore::IntData>( value, "option", name ) )
				{
					m_aaSeed = d->readable();
				}
				return;
			}
			else if( name == g_sampleMotionOptionName )
			{
				if( value == nullptr )
				{
					m_sampleMotion = boost::none;
				}
				else if( const IECore::BoolData *d = reportedCast<const IECore::BoolData>( value, "option", name ) )
				{
					m_sampleMotion = d->readable();
				}
				return;
			}
			else if( name == g_pluginSearchPathOptionName )
			{
				// We must include the OSL searchpaths in Arnold's shader
				// searchpaths so that the OSL shaders can be found.
				const char *searchPath = getenv( "OSL_SHADER_PATHS" );
				std::string s( searchPath ? searchPath : "" );
				if( value )
				{
					if( const IECore::StringData *d = reportedCast<const IECore::StringData>( value, "option", name ) )
					{
						s = d->readable() + ":" + s;
					}
				}
				AiNodeSetStr( options, g_pluginSearchPathArnoldString, AtString( s.c_str() ) );
				return;
			}
			else if( name == g_atmosphereOptionName )
			{
				m_atmosphere = nullptr;
				if( value )
				{
					if( const IECoreScene::ShaderNetwork *d = reportedCast<const IECoreScene::ShaderNetwork>( value, "option", name ) )
					{
						m_atmosphere = m_shaderCache->get( d );
					}
				}
				AiNodeSetPtr( options, g_atmosphereArnoldString, m_atmosphere ? m_atmosphere->root() : nullptr );
				return;
			}
			else if( name == g_backgroundOptionName )
			{
				m_background = nullptr;
				if( value )
				{
					if( const IECoreScene::ShaderNetwork *d = reportedCast<const IECoreScene::ShaderNetwork>( value, "option", name ) )
					{
						m_background = m_shaderCache->get( d );
					}
				}
				AiNodeSetPtr( options, g_backgroundArnoldString, m_background ? m_background->root() : nullptr );
				return;
			}
			else if( boost::starts_with( name.c_str(), "ai:aov_shader:" ) )
			{
				m_aovShaders.erase( name );
				if( value )
				{
					if( const IECoreScene::ShaderNetwork *d = reportedCast<const IECoreScene::ShaderNetwork>( value, "option", name ) )
					{
						m_aovShaders[name] = m_shaderCache->get( d );
					}
				}

				AtArray *array = AiArrayAllocate( m_aovShaders.size(), 1, AI_TYPE_NODE );
				int i = 0;
				for( AOVShaderMap::const_iterator it = m_aovShaders.begin(); it != m_aovShaders.end(); ++it )
				{
					AiArraySetPtr( array, i++, it->second->root() );
				}
				AiNodeSetArray( options, g_aovShadersArnoldString, array );
				return;
			}
			else if( boost::starts_with( name.c_str(), "ai:declare:" ) )
			{
				AtString arnoldName( name.c_str() + 11 );
				const AtParamEntry *parameter = AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( options ), arnoldName );
				if( parameter )
				{
					IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer::option", boost::format( "Unable to declare existing option \"%s\"." ) % arnoldName.c_str() );
				}
				else
				{
					const AtUserParamEntry *userParameter = AiNodeLookUpUserParameter( options, arnoldName );
					if( userParameter )
					{
						AiNodeResetParameter( options, arnoldName );
					}
					const IECore::Data *dataValue = IECore::runTimeCast<const IECore::Data>( value );
					if( dataValue )
					{
						ParameterAlgo::setParameter( options, arnoldName, dataValue );
					}
				}
				return;
			}
			else if( boost::starts_with( name.c_str(), "ai:" ) )
			{
				AtString arnoldName( name.c_str() + 3 );
				const AtParamEntry *parameter = AiNodeEntryLookUpParameter( AiNodeGetNodeEntry( options ), arnoldName );
				if( parameter )
				{
					const IECore::Data *dataValue = IECore::runTimeCast<const IECore::Data>( value );
					if( dataValue )
					{
						ParameterAlgo::setParameter( options, arnoldName, dataValue );
					}
					else
					{
						AiNodeResetParameter( options, arnoldName );
					}
					return;
				}
			}
			else if( boost::starts_with( name.c_str(), "user:" ) )
			{
				AtString arnoldName( name.c_str() );
				const IECore::Data *dataValue = IECore::runTimeCast<const IECore::Data>( value );
				if( dataValue )
				{
					ParameterAlgo::setParameter( options, arnoldName, dataValue );
				}
				else
				{
					AiNodeResetParameter( options, arnoldName );
				}
				return;
			}
			else if( boost::contains( name.c_str(), ":" ) )
			{
				// Ignore options prefixed for some other renderer.
				return;
			}

			IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer::option", boost::format( "Unknown option \"%s\"." ) % name.c_str() );
		}

		void output( const IECore::InternedString &name, const IECoreScene::Output *output )
		{
			m_outputs.erase( name );
			if( output )
			{
				try
				{
					m_outputs[name] = new ArnoldOutput( name, output, nodeDeleter( m_renderType ) );
				}
				catch( const std::exception &e )
				{
					IECore::msg( IECore::Msg::Warning, "IECoreArnold::Renderer::output", e.what() );
				}
			}

		}

		// Some of Arnold's globals come from camera parameters, so the
		// ArnoldRenderer calls this method to notify the ArnoldGlobals
		// of each camera as it is created.
		void camera( const std::string &name, IECoreScene::ConstCameraPtr camera )
		{
			m_cameras[name] = camera;
		}

		void render()
		{
			updateCameraMeshes();

			AiNodeSetInt(
				AiUniverseGetOptions(), g_aaSeedArnoldString,
				m_aaSeed.get_value_or( m_frame.get_value_or( 1 ) )
			);


			// Do the appropriate render based on
			// m_renderType.
			switch( m_renderType )
			{
				case IECoreScenePreview::Renderer::Batch :
				{
					// Loop through all cameras referenced by any current outputs,
					// and do a render for each
					std::set<std::string> cameraOverrides;
					for( const auto &it : m_outputs )
					{
						cameraOverrides.insert( it.second->cameraOverride() );
					}

					for( const auto &cameraOverride : cameraOverrides )
					{
						updateCamera( cameraOverride.size() ? cameraOverride : m_cameraName );
						const int result = AiRender( AI_RENDER_MODE_CAMERA );
						if( result != AI_SUCCESS )
						{
							throwError( result );
						}
					}
					break;
				}
				case IECoreScenePreview::Renderer::SceneDescription :
					// An ASS file can only contain options to render from one camera,
					// so just use the default camera
					updateCamera( m_cameraName );
					AiASSWrite( m_assFileName.c_str(), AI_NODE_ALL );
					break;
				case IECoreScenePreview::Renderer::Interactive :
					// If we want to use Arnold's progressive refinement, we can't be constantly switching
					// the camera around, so just use the default camera
					updateCamera( m_cameraName );
					m_interactiveRenderController.setRendering( true );

					break;
			}
		}

		void pause()
		{
			m_interactiveRenderController.setRendering( false );
		}

	private :

		void throwError( int errorCode )
		{
			switch( errorCode )
			{
				case AI_ABORT :
					throw IECore::Exception( "Render aborted" );
				case AI_ERROR_NO_CAMERA :
					throw IECore::Exception( "Camera not defined" );
				case AI_ERROR_BAD_CAMERA :
					throw IECore::Exception( "Bad camera" );
				case AI_ERROR_VALIDATION :
					throw IECore::Exception( "Usage not validated" );
				case AI_ERROR_RENDER_REGION :
					throw IECore::Exception( "Invalid render region" );
				case AI_INTERRUPT :
					throw IECore::Exception( "Render interrupted by user" );
				case AI_ERROR_NO_OUTPUTS :
					throw IECore::Exception( "No outputs" );
				case AI_ERROR :
					throw IECore::Exception( "Generic Arnold error" );
			}
		}

		bool updateLogFlags( const std::string name, const IECore::Data *value, bool console )
		{
			int flagToModify = AI_LOG_NONE;
			if( name == "info" )
			{
				flagToModify = AI_LOG_INFO;
			}
			else if( name == "warnings" )
			{
				flagToModify = AI_LOG_WARNINGS;
			}
			else if( name == "errors" )
			{
				flagToModify = AI_LOG_ERRORS;
			}
			else if( name == "debug" )
			{
				flagToModify = AI_LOG_DEBUG;
			}
			else if( name == "stats" )
			{
				flagToModify = AI_LOG_STATS;
			}
			else if( name == "ass_parse" )
			{
				flagToModify = AI_LOG_ASS_PARSE;
			}
			else if( name == "plugins" )
			{
				flagToModify = AI_LOG_PLUGINS;
			}
			else if( name == "progress" )
			{
				flagToModify = AI_LOG_PROGRESS;
			}
			else if( name == "nan" )
			{
				flagToModify = AI_LOG_NAN;
			}
			else if( name == "timestamp" )
			{
				flagToModify = AI_LOG_TIMESTAMP;
			}
			else if( name == "backtrace" )
			{
				flagToModify = AI_LOG_BACKTRACE;
			}
			else if( name == "memory" )
			{
				flagToModify = AI_LOG_MEMORY;
			}
			else if( name == "color" )
			{
				flagToModify = AI_LOG_COLOR;
			}
			else
			{
				return false;
			}

			bool turnOn = false;
			if( value == nullptr )
			{
				turnOn = flagToModify & ( console == false ? g_logFlagsDefault : g_consoleFlagsDefault );
			}
			else if( const IECore::BoolData *d = reportedCast<const IECore::BoolData>( value, "option", name ) )
			{
				turnOn = d->readable();
			}
			else
			{
				return true;
			}

			int &flags = console ? m_consoleFlags : m_logFileFlags;
			if( turnOn )
			{
				flags |= flagToModify;
			}
			else
			{
				flags = flags & ~flagToModify;
			}

			if( console )
			{
				AiMsgSetConsoleFlags( flags );
			}
			else
			{
				AiMsgSetLogFileFlags( flags );
			}

			return true;
		}

		void updateCamera( const std::string &cameraName )
		{
			AtNode *options = AiUniverseGetOptions();

			// Set the global output list in the options to all outputs matching the current camera
			IECore::StringVectorDataPtr outputs = new IECore::StringVectorData;
			IECore::StringVectorDataPtr lpes = new IECore::StringVectorData;
			for( OutputMap::const_iterator it = m_outputs.begin(), eIt = m_outputs.end(); it != eIt; ++it )
			{
				std::string outputCamera = it->second->cameraOverride();
				if( outputCamera == "" )
				{
					outputCamera = m_cameraName;
				}

				if( outputCamera == cameraName )
				{
					it->second->append( outputs->writable(), lpes->writable() );
				}
			}
			IECoreArnold::ParameterAlgo::setParameter( options, "outputs", outputs.get() );
			IECoreArnold::ParameterAlgo::setParameter( options, "light_path_expressions", lpes.get() );


			const IECoreScene::Camera *cortexCamera;
			AtNode *arnoldCamera = AiNodeLookUpByName( AtString( cameraName.c_str() ) );
			if( arnoldCamera )
			{
				cortexCamera = m_cameras[cameraName].get();
				m_defaultCamera = nullptr;
			}
			else
			{
				if( !m_defaultCamera )
				{
					IECoreScene::ConstCameraPtr defaultCortexCamera = new IECoreScene::Camera();
					m_cameras["ieCoreArnold:defaultCamera"] = defaultCortexCamera;
					m_defaultCamera = SharedAtNodePtr(
						CameraAlgo::convert( defaultCortexCamera.get(), "ieCoreArnold:defaultCamera", nullptr ),
						nodeDeleter( m_renderType )
					);
				}
				cortexCamera = m_cameras["ieCoreArnold:defaultCamera"].get();
				arnoldCamera = m_defaultCamera.get();
			}
			AiNodeSetPtr( options, g_cameraArnoldString, arnoldCamera );

			Imath::V2i resolution = cortexCamera->renderResolution();
			Imath::Box2i renderRegion = cortexCamera->renderRegion();

			AiNodeSetInt( options, g_xresArnoldString, resolution.x );
			AiNodeSetInt( options, g_yresArnoldString, resolution.y );

			AiNodeSetFlt( options, g_pixelAspectRatioArnoldString, cortexCamera->getPixelAspectRatio() );

			if(
				renderRegion.min.x >= renderRegion.max.x ||
				renderRegion.min.y >= renderRegion.max.y
			)
			{
				// Arnold does not permit empty render regions.  The user intent of an empty render
				// region is probably to render as little as possible ( it could happen if you
				// built a tool to crop to an object which passed out of frame ).
				// We just pick one pixel in the corner
				renderRegion = Imath::Box2i( Imath::V2i( 0 ), Imath::V2i( 1 ) );
			}

			// Note that we have to flip Y and subtract 1 from the max value, because
			// renderRegion is stored in Gaffer image format ( +Y up and an exclusive upper bound )
			AiNodeSetInt( options, g_regionMinXArnoldString, renderRegion.min.x );
			AiNodeSetInt( options, g_regionMinYArnoldString, resolution.y - renderRegion.max.y );
			AiNodeSetInt( options, g_regionMaxXArnoldString, renderRegion.max.x - 1 );
			AiNodeSetInt( options, g_regionMaxYArnoldString, resolution.y - renderRegion.min.y - 1 );

			Imath::V2f shutter = cortexCamera->getShutter();
			if( m_sampleMotion.get_value_or( true ) )
			{
				AiNodeSetFlt( arnoldCamera, g_shutterStartArnoldString, shutter[0] );
				AiNodeSetFlt( arnoldCamera, g_shutterEndArnoldString, shutter[1] );
			}
			else
			{
				AiNodeSetFlt( arnoldCamera, g_shutterStartArnoldString, shutter[0] );
				AiNodeSetFlt( arnoldCamera, g_shutterEndArnoldString, shutter[0] );
			}
		}

		void updateCameraMeshes()
		{
			for( const auto &it : m_cameras )
			{
				IECoreScene::ConstCameraPtr cortexCamera = it.second;

				std::string meshPath = parameter( cortexCamera->parameters(), "mesh", std::string("") );
				if( !meshPath.size() )
				{
					continue;
				}

				AtNode *arnoldCamera = AiNodeLookUpByName( AtString( it.first.c_str() ) );
				if( !arnoldCamera )
				{
					continue;
				}

				AtNode *meshNode = AiNodeLookUpByName( AtString( meshPath.c_str() ) );
				if( meshNode )
				{
					AtString meshType = AiNodeEntryGetNameAtString( AiNodeGetNodeEntry( meshNode ) );
					if( meshType == g_ginstanceArnoldString )
					{
						AiNodeSetPtr( arnoldCamera, g_meshArnoldString, AiNodeGetPtr( meshNode, g_nodeArnoldString ) );
						AiNodeSetMatrix( arnoldCamera, g_matrixArnoldString, AiNodeGetMatrix( meshNode, g_matrixArnoldString ) );
						continue;
					}
					else if( meshType == g_polymeshArnoldString )
					{
						AiNodeSetPtr( arnoldCamera, g_meshArnoldString, meshNode );
						AiNodeSetMatrix( arnoldCamera, g_matrixArnoldString, AiM4Identity() );
						continue;
					}
				}

				throw IECore::Exception( boost::str( boost::format( "While outputting camera \"%s\", could not find target mesh at \"%s\"" ) % it.first % meshPath ) );
			}
		}


		// Members used by all render types

		IECoreScenePreview::Renderer::RenderType m_renderType;

		IECoreArnold::UniverseBlock m_universeBlock;

		typedef std::map<IECore::InternedString, ArnoldOutputPtr> OutputMap;
		OutputMap m_outputs;

		typedef std::map<IECore::InternedString, ArnoldShaderPtr> AOVShaderMap;
		AOVShaderMap m_aovShaders;

		ArnoldShaderPtr m_atmosphere;
		ArnoldShaderPtr m_background;

		std::string m_cameraName;
		typedef tbb::concurrent_unordered_map<std::string, IECoreScene::ConstCameraPtr> CameraMap;
		CameraMap m_cameras;
		SharedAtNodePtr m_defaultCamera;

		int m_logFileFlags;
		int m_consoleFlags;
		boost::optional<int> m_frame;
		boost::optional<int> m_aaSeed;
		boost::optional<bool> m_sampleMotion;
		ShaderCache *m_shaderCache;

		// Members used by interactive renders

		InteractiveRenderController m_interactiveRenderController;

		// Members used by ass generation "renders"

		std::string m_assFileName;

};

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldRendererBase definition
//////////////////////////////////////////////////////////////////////////

namespace
{

ArnoldRendererBase::ArnoldRendererBase( NodeDeleter nodeDeleter, LightFilterConnectionsPtr lightFilterConnections, AtNode *parentNode )
	:	m_nodeDeleter( nodeDeleter ),
		m_shaderCache( new ShaderCache( nodeDeleter, parentNode ) ),
		m_instanceCache( new InstanceCache( nodeDeleter, parentNode ) ),
		m_lightListCache( new LightListCache() ),
		m_connections( lightFilterConnections ),
		m_parentNode( parentNode )
{
}

ArnoldRendererBase::~ArnoldRendererBase()
{
}

IECore::InternedString ArnoldRendererBase::name() const
{
	return "Arnold";
}

ArnoldRendererBase::AttributesInterfacePtr ArnoldRendererBase::attributes( const IECore::CompoundObject *attributes )
{
	return new ArnoldAttributes( attributes, m_shaderCache.get(), m_lightListCache.get(), m_connections.get() );
}

ArnoldRendererBase::ObjectInterfacePtr ArnoldRendererBase::camera( const std::string &name, const IECoreScene::Camera *camera, const AttributesInterface *attributes )
{
	Instance instance = m_instanceCache->get( camera, attributes, name );

	ObjectInterfacePtr result = new ArnoldObject( instance );
	result->attributes( attributes );
	return result;
}

ArnoldRendererBase::ObjectInterfacePtr ArnoldRendererBase::light( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes )
{
	Instance instance = m_instanceCache->get( object, attributes, name );
	ObjectInterfacePtr result = new ArnoldLight( name, instance, m_nodeDeleter, m_parentNode, m_connections.get() );
	result->attributes( attributes );
	return result;
}

ArnoldRendererBase::ObjectInterfacePtr ArnoldRendererBase::lightFilter( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes )
{
	Instance instance = m_instanceCache->get( object, attributes, name );
	ObjectInterfacePtr result = new ArnoldLightFilter( name, instance, m_nodeDeleter, m_parentNode, m_connections.get() );
	result->attributes( attributes );

	return result;
}

ArnoldRendererBase::ObjectInterfacePtr ArnoldRendererBase::object( const std::string &name, const IECore::Object *object, const AttributesInterface *attributes )
{
	Instance instance = m_instanceCache->get( object, attributes, name );
	ObjectInterfacePtr result = new ArnoldObject( instance );
	result->attributes( attributes );
	return result;
}

ArnoldRendererBase::ObjectInterfacePtr ArnoldRendererBase::object( const std::string &name, const std::vector<const IECore::Object *> &samples, const std::vector<float> &times, const AttributesInterface *attributes )
{
	Instance instance = m_instanceCache->get( samples, times, attributes, name );
	ObjectInterfacePtr result = new ArnoldObject( instance );
	result->attributes( attributes );
	return result;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// ArnoldRenderer
//////////////////////////////////////////////////////////////////////////

namespace
{

/// The full renderer implementation as presented to the outside world.
class ArnoldRenderer final : public ArnoldRendererBase
{

	public :

		ArnoldRenderer( RenderType renderType, const std::string &fileName )
			:	ArnoldRendererBase( nodeDeleter( renderType ), LightFilterConnectionsPtr( new LightFilterConnections( renderType != IECoreScenePreview::Renderer::Interactive ) ) ),
				m_globals( new ArnoldGlobals( renderType, fileName, m_shaderCache.get() ) )
		{
		}

		~ArnoldRenderer() override
		{
			pause();
		}

		void option( const IECore::InternedString &name, const IECore::Object *value ) override
		{
			m_globals->option( name, value );
		}

		void output( const IECore::InternedString &name, const IECoreScene::Output *output ) override
		{
			m_globals->output( name, output );
		}

		ObjectInterfacePtr camera( const std::string &name, const IECoreScene::Camera *camera, const AttributesInterface *attributes ) override
		{
			m_globals->camera( name, camera );
			return ArnoldRendererBase::camera( name, camera, attributes );
		}

		void render() override
		{
			m_shaderCache->clearUnused();
			m_instanceCache->clearUnused();
			m_lightListCache->clear();
			m_connections->update();
			m_globals->render();
		}

		void pause() override
		{
			m_globals->pause();
		}

	private :

		std::unique_ptr<ArnoldGlobals> m_globals;

		// Registration with factory

		static Renderer::TypeDescription<ArnoldRenderer> g_typeDescription;

};

IECoreScenePreview::Renderer::TypeDescription<ArnoldRenderer> ArnoldRenderer::g_typeDescription( "Arnold" );

} // namespace
