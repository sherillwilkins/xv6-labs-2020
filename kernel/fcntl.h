#define O_RDONLY   0x000
#define O_WRONLY   0x001
#define O_RDWR     0x002
#define O_CREATE   0x200
#define O_TRUNC    0x400
// 新增的标志位不能与已存在的标志位重叠
#define O_NOFOLLOW 0x004
