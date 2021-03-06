//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2017, Image Engine Design Inc. All rights reserved.
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

#include "GafferArnold/ArnoldAOVShader.h"

using namespace IECore;
using namespace Gaffer;
using namespace GafferScene;
using namespace GafferArnold;

GAFFER_GRAPHCOMPONENT_DEFINE_TYPE( ArnoldAOVShader );

size_t ArnoldAOVShader::g_firstPlugIndex = 0;

ArnoldAOVShader::ArnoldAOVShader( const std::string &name )
	:	GlobalShader( name )
{
	storeIndexOfNextChild( g_firstPlugIndex );
	addChild( new StringPlug( "optionSuffix", Gaffer::Plug::In, "custom" ) );
}

ArnoldAOVShader::~ArnoldAOVShader()
{
}

Gaffer::StringPlug *ArnoldAOVShader::optionSuffixPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex );
}

const Gaffer::StringPlug *ArnoldAOVShader::optionSuffixPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex );
}

bool ArnoldAOVShader::affectsOptionName( const Gaffer::Plug *input ) const
{
	return input == optionSuffixPlug();
}

void ArnoldAOVShader::hashOptionName( const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	optionSuffixPlug()->hash( h );
}

std::string ArnoldAOVShader::computeOptionName( const Gaffer::Context *context ) const
{
	return "ai:aov_shader:" + optionSuffixPlug()->getValue();
}
