namespace ParaViewLink
{
    /// <summary>
    /// Message type constants matching UE5MeshSender.py MSG_TYPE_* values.
    /// Wire format (little-endian):
    ///   int32  payload_byte_count
    ///   int32  message_type
    ///   byte[] payload
    ///   int32  ack sent back (0 = OK)
    /// </summary>
    public static class MeshCmd
    {
        public const int String     =  0;
        public const int Mesh       =  1;
        public const int Update     =  2;
        public const int Scalars    =  3;
        public const int Variable   =  4;
        public const int Colormap   =  5;
        public const int Ping       =  6;

        /// <summary>All-in-one: geometry + scalars + range + names.</summary>
        public const int PVMesh     = 10;

        /// <summary>Computational domain AABB: float32[6] xmin xmax ymin ymax zmin zmax.</summary>
        public const int Bounds     = 11;

        /// <summary>Show/hide a named mesh actor.</summary>
        public const int Visibility = 12;
    }
}
