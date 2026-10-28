#include <simulator/g3_draw.h>

#include <simulator/g3_handler.h>
#include <simulator/assert.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <memory>

#include <nitro.h>
#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <unordered_map>
#include <simulator/sim_crc32.h>
#include "simulator/sim_g3_TextureCache.hpp"
#include "simulator/sim_g3_Texture.hpp"
#include "simulator/sim_texture_packs.h"

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#define G3_DRAW_MAX_ITEMS 1000

#define getbit(x,n) ( ( (x) >> (n) ) & 1 )

typedef struct {
	float zIdx;
	GLuint textureId;
	G3SIM_PolygonAttr_t polygonAttr;
	G3SIM_TexImageParam_t texImageParam;
	G3SIM_Vertex_t * verts;
	u32 vertsCount;
} g3_draw_item_t;

G3SIM_Vertex_t s_G3DrawVerts[G3_DRAW_MAX_VERTS];

// GPU Texture Cache
static SIM::G3::TextureCache sTextureCache;

//The item list is used for drawing translucent things sorted by Z position
static g3_draw_item_t s_G3DrawItemList[G3_DRAW_MAX_ITEMS];
static u32 s_G3DrawItemListCount = 0;

static u32 s_G3DrawCurVertIdx = 0;
static GLuint s_G3DrawVertexArray;
static GLuint s_G3DrawVertexBuffer;

extern G3SIM_TexImageParam_t s_texImageParam;
extern G3SIM_PolygonAttr_t s_curPolygonAttr;
extern GLuint g3shaderProgramID;
extern u8 s_SIM_g3tex[4*1024*1024];
extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;

static void* getTextureVramBank();
static void* getTexPlttVramBank();

static void* getTextureVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTex ){
		case GX_VRAM_TEX_0_A:
		case GX_VRAM_TEX_01_AB:
		case GX_VRAM_TEX_012_ABC:
		case GX_VRAM_TEX_0123_ABCD:
		case GX_VRAM_TEX_01_AC:
		case GX_VRAM_TEX_01_AD:
		case GX_VRAM_TEX_012_ABD:
		case GX_VRAM_TEX_012_ACD:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		case GX_VRAM_TEX_0_B:
		case GX_VRAM_TEX_01_BC:
		case GX_VRAM_TEX_012_BCD:
		case GX_VRAM_TEX_01_BD:
			vramBank = (void *)HW_LCDC_VRAM_B;
			break;
		case GX_VRAM_TEX_0_C:
		case GX_VRAM_TEX_01_CD:
			vramBank = (void *)HW_LCDC_VRAM_C;
			break;
		case GX_VRAM_TEX_0_D:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		default:
			//?
			break;
	}
	
	return vramBank;
}

static void* getTexPlttVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTexPltt ) {
		case GX_VRAM_TEXPLTT_NONE:
		case GX_VRAM_TEXPLTT_0123_E:
		case GX_VRAM_TEXPLTT_01234_EF:
		case GX_VRAM_TEXPLTT_012345_EFG:
			vramBank = (void *)HW_LCDC_VRAM_E;
			break;
		case GX_VRAM_TEXPLTT_0_F:
		case GX_VRAM_TEXPLTT_01_FG:
			vramBank = (void *)HW_LCDC_VRAM_F;
			break;
		case GX_VRAM_TEXPLTT_0_G:
			vramBank = (void *)HW_LCDC_VRAM_G;
			break;
	}
	
	return vramBank;
}

// Vertex data is uploaded once per draw batch, hundreds of times a frame. Rewriting the
// same buffer in place makes tiled mobile GPUs wait for (or copy) the previous batch
// still using it; giving the buffer new storage each time ("orphaning") never stalls.
static void G3SIM_UploadVerts(const G3SIM_Vertex_t *verts, int count)
{
	glBufferData(GL_ARRAY_BUFFER, sizeof(G3SIM_Vertex_t) * count, verts, GL_STREAM_DRAW);
}

// Uniform locations never change once the shader is linked; looking them up by name for
// every draw is pure overhead.
static GLint G3SIM_Uniform(const char *name)
{
	struct CachedUniform { const char *name; GLint location; };
	static CachedUniform sCache[16];
	static int sCount = 0;
	static GLuint sProgram = 0;

	if (sProgram != g3shaderProgramID) {
		sProgram = g3shaderProgramID;
		sCount = 0;
	}
	for (int i = 0; i < sCount; i++) {
		if (sCache[i].name == name || strcmp(sCache[i].name, name) == 0) {
			return sCache[i].location;
		}
	}
	GLint location = glGetUniformLocation(g3shaderProgramID, name);
	if (sCount < (int)(sizeof(sCache) / sizeof(sCache[0]))) {
		sCache[sCount].name = name;
		sCache[sCount].location = location;
		sCount++;
	}
	return location;
}

void G3SIM_AddVtx(G3SIM_Vertex_t * vtx)
{
	memcpy(&s_G3DrawVerts[s_G3DrawCurVertIdx], vtx, sizeof(G3SIM_Vertex_t));

	s_G3DrawCurVertIdx = s_G3DrawCurVertIdx + 1;

	SIM_assert(s_G3DrawCurVertIdx < G3_DRAW_MAX_VERTS);
	return;
}

static u64 sG3SubmitNs = 0;
static u32 sG3Draws = 0;

u32 G3SIM_TakeDrawCount()
{
	u32 draws = sG3Draws;
	sG3Draws = 0;
	return draws;
}

static u64 G3SIM_NowNs()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
}

u64 G3SIM_TakeSubmitNs()
{
	u64 ns = sG3SubmitNs;
	sG3SubmitNs = 0;
	return ns;
}

void G3SIM_DrawArray()
{
	if( s_G3DrawCurVertIdx != 0 )
	{
		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		G3SIM_UploadVerts(s_G3DrawVerts, s_G3DrawCurVertIdx);
		glDrawArrays(GL_TRIANGLES,0,s_G3DrawCurVertIdx);
		sG3Draws++;
	}
	return;
}

void G3SIM_DrawCleanUp()
{
	s_G3DrawCurVertIdx = 0;
}

void G3SIM_DrawInit()
{
	glGenVertexArrays(1, &s_G3DrawVertexArray);
	glBindVertexArray(s_G3DrawVertexArray);

	glGenBuffers(1, &s_G3DrawVertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(s_G3DrawVerts), NULL, GL_DYNAMIC_DRAW);


	glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, s));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, r));

	glBindVertexArray(0);
	glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}

static void G3SIM_FlushArrayInner();

void G3SIM_FlushArray()
{
	if(s_G3DrawCurVertIdx == 0) {
		// No verts to draw
		return;
	}
	u64 start = G3SIM_NowNs();
	G3SIM_FlushArrayInner();
	sG3SubmitNs += G3SIM_NowNs() - start;
}

static void G3SIM_FlushArrayInner()
{

	#ifdef SDK_TRACY_ENABLE
	TracyCZone(FlushArrayZone, 1);
	#endif

	u8 * texBuf = nullptr;
	std::shared_ptr<SIM::G3::Texture> texture = nullptr;

	if( s_texImageParam.textureFormat != GX_TEXFMT_NONE )
	{
		GLuint g3TextureId;
		u8 * vramTex = (u8*)((uPtr)getTextureVramBank() + s_texImageParam.textureOffset);
		u8 * vramPltt = (u8*)((uPtr)getTexPlttVramBank() + s_texPlttBase);
		u16 * colorAddr = (u16*)vramPltt;

		//Calculate the texture + palette CRC
		u32 paletteCRC = SIM_crc32buf(vramPltt, 512);
		u32 vramTexBufSize = 0;

		if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT4) {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize) >> 2;
		} else if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT16) {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize) >> 1;
		} else {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize);
		}

		u32 textureCRC = SIM_crc32buf(vramTex, vramTexBufSize);
		// The same data drawn with colour 0 transparent decodes differently
		if (s_texImageParam.color0) {
			paletteCRC ^= 0x9E3779B9;
		}
		texture = sTextureCache.GetTexture(textureCRC, paletteCRC);

		// A texture pack's replacement (simulator/sim_texture_packs.h)
		std::string packName;
		if (texture == nullptr && s_texImageParam.textureFormat != GX_TEXFMT_COMP4x4
		    && (SIM::TexturePacks::Replacing() || SIM::TexturePacks::Dumping())) {
			packName = SIM::TexturePacks::Name(s_texImageParam.textureFormat, s_texImageParam.textureSSize,
			                                   s_texImageParam.textureTSize, textureCRC, vramPltt,
			                                   s_texImageParam.color0 != 0);
			if (SIM::TexturePacks::Replacing()) {
				texture = SIM::TexturePacks::Replacement(packName);
				if (texture != nullptr) {
					sTextureCache.AddTexture(textureCRC, paletteCRC, texture);
				}
			}
		}

		if(texture == nullptr) {
			// decode texture and create it in the cache
			//Convert the DS texture data into a format opengl can understand
			u8 * outTexBuf = new u8[4*s_texImageParam.textureSSize * s_texImageParam.textureTSize];
			memset((void*)outTexBuf, 0, 4*s_texImageParam.textureSSize * s_texImageParam.textureTSize);


			switch( s_texImageParam.textureFormat ){
				case GX_TEXFMT_A3I5:
					G3SIM_DecodeTexA3I5(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT4:
					G3SIM_DecodeTex4(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT16:
					G3SIM_DecodeTex16(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT256:
					G3SIM_DecodeTex256(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_COMP4x4:
					SIM_assert_always();
					//TODO
					break;
				case GX_TEXFMT_A5I3:
					G3SIM_DecodeTexA5I3(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_DIRECT:
					G3SIM_DecodeTexDirect(vramTex, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
			}

			// This creates a GL texture and uploads it to the GPU with glTexImage2D
			texture = std::make_shared<SIM::G3::Texture>(s_texImageParam.textureTSize, s_texImageParam.textureSSize, outTexBuf);

			if (!packName.empty() && SIM::TexturePacks::Dumping()) {
				SIM::TexturePacks::Dump(packName, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
			}

			sTextureCache.AddTexture(textureCRC, paletteCRC, texture);

			// Texture buffer can be freed now, it has been sent to the GPU
			delete[] outTexBuf;
		}

		texture->Activate();

		if(s_texImageParam.repeatS) {
			if(s_texImageParam.flipS)
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
			} else {
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			}
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		}

		if(s_texImageParam.repeatT) {
			if(s_texImageParam.flipT)
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
			} else {
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			}
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		}


		GLint texUnitLoc = G3SIM_Uniform("myTexture");
		//set texture 0 in the shader
		glProgramUniform1i(g3shaderProgramID, texUnitLoc , 0);

		//Enable texture in the shader
		glProgramUniform1i(g3shaderProgramID, G3SIM_Uniform("useTexture") , 1);
	}
	else
	{
		//Disable texture in the shader
		glProgramUniform1i(g3shaderProgramID, G3SIM_Uniform("useTexture") , 0);							
	}

	// Set the polygon mode in the shader
	GLint polygonModeLoc = G3SIM_Uniform("polygonMode");
	glProgramUniform1i(g3shaderProgramID, polygonModeLoc , s_curPolygonAttr.polygonMode);

	// Set the fog attributes in the shader
	GLint useFogLoc = G3SIM_Uniform("useFog");
	int useFog = getbit(s_reg_G3X_DISP3DCNT, 7);
	if(!s_curPolygonAttr.fogEnable) {
		useFog = 0;
	}
	glProgramUniform1i(g3shaderProgramID, useFogLoc , useFog);
	if(useFog) {
		// Fog color
		GLint fogColorLoc = G3SIM_Uniform("fogColor");
		u32 fogRed = s_reg_G3X_FOG_COLOR & 0b11111;
		u32 fogGreen = (s_reg_G3X_FOG_COLOR >> 5) & 0b11111;
		u32 fogBlue = (s_reg_G3X_FOG_COLOR >> 10) & 0b11111;
		u32 fogAlpha = (s_reg_G3X_FOG_COLOR >> 16) & 0b11111;
		float fogRedF = (float)fogRed / 32.0f;
		float fogGreenF = (float)fogGreen / 32.0f;
		float fogBlueF = (float)fogBlue / 32.0f;
		float fogAlphaF = (float)fogAlpha / 32.0f;
		glProgramUniform4f(g3shaderProgramID, fogColorLoc, fogRedF, fogGreenF, fogBlueF, fogAlphaF);


		u32 fogShift = (s_reg_G3X_DISP3DCNT >> 8) & 0b1111;
		u32 fogStep = 0x400 >> fogShift;

		//Fog depth boundaries
		u32 fogOffset = s_reg_G3X_FOG_OFFSET;

		float fogDepthBoundary[32];
		float fogDensity[32];
		u8 * fogTablePtr = (u8*)&s_reg_G3X_FOG_TABLE_0;
		for(int i=0; i < 32; i++) {
			fogDepthBoundary[i] = ((float)(fogOffset + fogStep*(i+1)) / 32767.0f) * 2.0f - 1.0f;
			fogDensity[i] = (float)fogTablePtr[i] / 127.0f;
		}

		GLint fogDepthBoundaryLoc = G3SIM_Uniform("fogDepthBoundary");
		glProgramUniform1fv(g3shaderProgramID, fogDepthBoundaryLoc, 32, fogDepthBoundary);

		GLint fogDensityLoc = G3SIM_Uniform("fogDensity");
		glProgramUniform1fv(g3shaderProgramID, fogDensityLoc, 32, fogDensity);
	}



	// If this is a translucent draw, save it for the end of the frame to render it last
	if(
		( s_texImageParam.textureFormat == GX_TEXFMT_A3I5 
	   || s_texImageParam.textureFormat == GX_TEXFMT_A5I3
	   || s_curPolygonAttr.alpha < 0.99f )
	 && s_G3DrawCurVertIdx != 0) {
		g3_draw_item_t * item = &s_G3DrawItemList[s_G3DrawItemListCount];

		memset(item, 0, sizeof(g3_draw_item_t));


		if(s_texImageParam.textureFormat != GX_TEXFMT_NONE) {
			//Allocate and store off the texture
			item->textureId = texture->GetTextureID();
		} else {
			item->textureId = 0;
		}

		//Copy over the polygonattr
		memcpy(&item->polygonAttr, &s_curPolygonAttr, sizeof(G3SIM_PolygonAttr_t));

		//Copy over the texImageParam
		memcpy(&item->texImageParam, &s_texImageParam, sizeof(G3SIM_TexImageParam_t));

		//Allocate and copy the verts
		item->verts = (G3SIM_Vertex_t*)(malloc(sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx));
		item->vertsCount = s_G3DrawCurVertIdx;
		memcpy(item->verts, s_G3DrawVerts, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx);

		s_G3DrawItemListCount++;
	} else {
		G3SIM_DrawArray();
	}

	G3SIM_DrawCleanUp();
	#ifdef SDK_TRACY_ENABLE
	TracyCZoneEnd(FlushArrayZone);
	#endif
}

void G3SIM_DrawItems()
{
	//TODO sort the items by Z coordinate


	//Draws all the translucent objects that were deferred to the end of the frame.
	for(int i=0; i<s_G3DrawItemListCount; i++) {
		g3_draw_item_t * item = &s_G3DrawItemList[i];
		if(item->textureId != 0){
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, item->textureId);
			//Enable texture in the shader
			glProgramUniform1i(g3shaderProgramID, G3SIM_Uniform("useTexture") , 1);
		} else {
			glProgramUniform1i(g3shaderProgramID, G3SIM_Uniform("useTexture") , 0);
		}

		if(item->texImageParam.flipS)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		}

		if(item->texImageParam.flipT)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		}

		// Set the polygon mode in the shader
		GLint polygonModeLoc = G3SIM_Uniform("polygonMode");
		glProgramUniform1i(g3shaderProgramID, polygonModeLoc, item->polygonAttr.polygonMode);

		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		G3SIM_UploadVerts(item->verts, item->vertsCount);
		glDrawArrays(GL_TRIANGLES,0, item->vertsCount);
		sG3Draws++;

		free(item->verts);
	}
	s_G3DrawItemListCount = 0;

	// Texture packs reloaded or switched: textures are made again from here on (nothing
	// drawn uses the old ones any more)
	if (SIM::TexturePacks::TakeChanged()) {
		sTextureCache.Clear();
	}
}