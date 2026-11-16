//=============================================================================
// UCNavNative - the routes on a map's walkable area, in C++ (native/NavRoute.h): A* over the polygons of a
// UCNavMesh, then the funnel. UCNavMesh.Route calls it. The package is DeusEdixNav, the same for every project and for
// the original game and Revision.
//
// Where the C++ runs:
//  - Windows: DeusEdixNav.dll, which must be next to the package in the game's System (without it the game's own
//    engine stops as soon as it loads the package: "'DeusEdixNav' is not a DLL package").
//  - macOS and Linux: there is no DLL (it is 32-bit Windows code). OpenDE, the engine that runs the editor and the
//    game there, answers this same class's Route itself (src/engine/nav_route.cc), so the package needs no library.
//=============================================================================
class UCNavNative extends Object
	native
	abstract;

// The route from From to To on the walkable area of Mesh (a UCNavMesh: its arrays are read where they are), into
// Mesh.Corner[0..]: the number of corners (To the last), 0 when there is no way on the walkable area.
native static final function int Route(Object Mesh, vector From, vector To);
