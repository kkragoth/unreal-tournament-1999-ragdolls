"""UT99 (v436) ULodMesh serialized-data parser (standalone debug tool).

The actual decoder lives in tools/extract_mesh.py (BlobReader + parse_mesh_blob);
this script dumps every field of the Commando mesh blob for manual verification.

Blob layout (in stream order, validated against Botpack.u export 3251):
    FName     objName            compact  (object's serialized name)
    FBox      boundingBox        25 bytes (Min, Max, bValid)
    FSphere   boundingSphere     16 bytes
    TLazyArray<FMeshVert>    Verts      [skipPos][compact count][inline data]
    TLazyArray<FMeshTri>     Tris
    TArray<FMeshAnimSeq>     AnimSeqs
    TLazyArray<FMeshVertConnect> Connects
    FBox  boundingBox2          (UPrimitive fields re-serialized)
    FSphere boundingSphere2
    TLazyArray<int>            VertLinks
    TArray<UTexture*>          Textures       (array of FName compacts)
    TArray<FBox>               BoundingBoxes  (25B each)
    TArray<FSphere>            BoundingSpheres (16B each)
    int VertexCount  (FrameVerts)
    int FrameCount   (AnimFrames)
    uint AndFlags, OrFlags
    FVector MeshScale
    FVector MeshOrigin
    FRotator RotOrigin          (3 x int32 in v436)
    int CurPoly, CurVertex
    TArray<float> TextureLOD             (ver>=66)
    [ULodMesh:]
    TArray<_WORD>   CollapsePointThus
    TArray<_WORD>   FaceLevel
    TArray<FMeshFace> Faces
    TArray<_WORD>   CollapseWedgeThus
    TArray<FMeshWedge> Wedges
    TArray<FMeshMaterial> Materials
    TArray<FMeshFace> SpecialFaces
    int ModelVerts, SpecialVerts
    float MeshScaleMax
    float LODHysteresis, LODStrength
    int LODMinVerts
    float LODMorph, LODZDisplace
    TArray<_WORD> RemapAnimVerts
    int OldFrameVerts
"""

import sys
sys.path.insert(0, r"C:\Users\kkragoth\dev\Ut99PubSrc\tools")
from extract_mesh import UTPackage, parse_mesh_blob


def main():
    pkg = UTPackage(r"C:\Users\kkragoth\Downloads\Unreal Tournament\Unreal Tournament\System\Botpack.u")
    e = pkg.exports[3251]
    blob = pkg.data[e.serial_offset:e.serial_offset + e.serial_size]
    res = parse_mesh_blob(pkg, blob, e.serial_offset)
    for nm, foff, _ in res["fields"]:
        print(f"{nm:<18} file_off={foff:>9}")
    print("---")
    for k, v in res.items():
        if k in ("fields",):
            continue
        print(f"{k}: {v}")
    print(f"end_pos={res['end_pos']} blob_size={res['blob_size']} "
          f"leftover={res['blob_size'] - res['end_pos']}")


if __name__ == "__main__":
    main()
