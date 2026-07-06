import numpy as np

# 1. Access the first element of the inputs list and pull its VTK object
input_item = inputs[0]
vtk_in = input_item.VTKObject

# 2. Copy the structure to the output dataset
output.ShallowCopy(vtk_in)

# 3. Get the 1D coordinate ranges per axis
if vtk_in.IsA('vtkImageData'):
    dims = vtk_in.GetDimensions()    # (nx, ny, nz)
    spacing = vtk_in.GetSpacing()    # (dx, dy, dz)
    origin = vtk_in.GetOrigin()      # (x0, y0, z0)

    x = np.arange(dims[0]) * spacing[0] + origin[0]
    y = np.arange(dims[1]) * spacing[1] + origin[1]
    z = np.arange(dims[2]) * spacing[2] + origin[2]

elif vtk_in.IsA('vtkRectilinearGrid'):
    x = [vtk_in.GetXCoordinates().GetValue(i) for i in range(vtk_in.GetXCoordinates().GetNumberOfValues())]
    y = [vtk_in.GetYCoordinates().GetValue(i) for i in range(vtk_in.GetYCoordinates().GetNumberOfValues())]
    z = [vtk_in.GetZCoordinates().GetValue(i) for i in range(vtk_in.GetZCoordinates().GetNumberOfValues())]

else:
    raise TypeError("Input must be a vtkImageData or vtkRectilinearGrid dataset.")

# 4. Explode the 1D arrays into 3D meshes using matrix indexing
x_mesh, y_mesh, z_mesh = np.meshgrid(x, y, z, indexing="ij")

# 5. Flatten them into the full point-count length (e.g., 1000 items)
x_array = x_mesh.ravel()
y_array = y_mesh.ravel()
z_array = z_mesh.ravel()

# 6. Push the arrays into ParaView"s PointData dictionary
output.PointData.append(x_array, "Coord_X")
output.PointData.append(y_array, "Coord_Y")
output.PointData.append(z_array, "Coord_Z")

