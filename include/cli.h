#ifndef nn_cli_H

// 视图定义
enum nn_cli_view_t
{
    NN_CLI_VIEW_USER,   // 用户视图
    NN_CLI_VIEW_CONFIG, // 配置视图
    NN_CLI_VIEW_BGP,    // BGP视图
    NN_CLI_VIEW_BMP,    // BMP视图
    NN_CLI_VIEW_RPKI,   // RPKI视图
    NN_CLI_VIEW_ALL     // 所有视图
};

#endif
