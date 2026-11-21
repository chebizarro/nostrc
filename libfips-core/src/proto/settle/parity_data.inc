/* Generated DATA by tests/tools/generate_settle_parity.py from
 * upstream 64f5b4d482cfe14abad108ffc2d92128f7658311. Do not hand-edit. */
static const bool settings_0[]={false,false};
static const fips_parity_transport transports_0[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_0[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_1[]={false,false};
static const fips_parity_transport transports_1[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_1[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_2[]={true,true};
static const fips_parity_transport transports_2[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_2[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_3[]={true,true};
static const fips_parity_transport transports_3[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_3[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_4[]={false,false};
static const fips_parity_transport transports_4[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_4[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_5[]={false,false};
static const fips_parity_transport transports_5[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_5[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_6[]={true,true};
static const fips_parity_transport transports_6[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_6[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_7[]={true,true};
static const fips_parity_transport transports_7[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_7[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_8[]={false,false};
static const fips_parity_transport transports_8[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_8[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_9[]={false,false};
static const fips_parity_transport transports_9[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_9[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_10[]={true,true};
static const fips_parity_transport transports_10[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_10[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_11[]={true,true};
static const fips_parity_transport transports_11[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_11[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_12[]={false,false};
static const fips_parity_transport transports_12[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_12[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_13[]={false,false};
static const fips_parity_transport transports_13[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_13[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_14[]={true,true};
static const fips_parity_transport transports_14[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_14[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_15[]={true,true};
static const fips_parity_transport transports_15[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_15[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_16[]={false,false};
static const fips_parity_transport transports_16[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_16[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_17[]={false,false};
static const fips_parity_transport transports_17[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_17[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_18[]={true,true};
static const fips_parity_transport transports_18[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_18[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_19[]={true,true};
static const fips_parity_transport transports_19[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_19[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_20[]={false,false};
static const fips_parity_transport transports_20[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_20[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_21[]={false,false};
static const fips_parity_transport transports_21[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_21[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_22[]={true,true};
static const fips_parity_transport transports_22[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_22[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_23[]={true,true};
static const fips_parity_transport transports_23[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_23[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(1000)},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_24[]={false,false};
static const fips_parity_transport transports_24[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_24[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_RESTART,.to=0},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_25[]={false,false};
static const fips_parity_transport transports_25[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_25[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_RESTART,.to=0},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_26[]={true,true};
static const fips_parity_transport transports_26[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_26[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_RESTART,.to=0},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_27[]={true,true};
static const fips_parity_transport transports_27[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_27[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_RESTART,.to=0},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_28[]={false,false};
static const fips_parity_transport transports_28[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_28[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_29[]={false,false};
static const fips_parity_transport transports_29[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_29[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_30[]={true,true};
static const fips_parity_transport transports_30[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_30[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_31[]={true,true};
static const fips_parity_transport transports_31[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_31[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_32[]={false,false};
static const fips_parity_transport transports_32[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_32[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_33[]={false,false};
static const fips_parity_transport transports_33[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_33[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_34[]={true,true};
static const fips_parity_transport transports_34[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_34[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_35[]={true,true};
static const fips_parity_transport transports_35[]={FIPS_PARITY_UDP};
static const fips_parity_step steps_35[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_36[]={false,false};
static const fips_parity_transport transports_36[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_36[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_37[]={false,false};
static const fips_parity_transport transports_37[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_37[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_38[]={true,true};
static const fips_parity_transport transports_38[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_38[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_39[]={true,true};
static const fips_parity_transport transports_39[]={FIPS_PARITY_TCP};
static const fips_parity_step steps_39[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_DIAL,.from=1,.to=0,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_CONNECTED,.to=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_REKEY,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_40[]={false,false};
static const fips_parity_transport transports_40[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_40[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_41[]={false,false};
static const fips_parity_transport transports_41[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_41[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_42[]={true,true};
static const fips_parity_transport transports_42[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_42[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_43[]={true,true};
static const fips_parity_transport transports_43[]={FIPS_PARITY_UDP,FIPS_PARITY_TCP};
static const fips_parity_step steps_43[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_UDP},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=1},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=1},
    {.kind=FIPS_PARITY_HEAL},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_TCP},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=1,.msg2=false,.nth=2},
    {.kind=FIPS_PARITY_HANDSHAKE,.to=0,.msg2=true,.nth=2},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_44[]={true,true};
static const fips_parity_transport transports_44[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_44[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(5000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(25000)},
    {.kind=FIPS_PARITY_HEAL},
};
static const bool settings_45[]={true,true};
static const fips_parity_transport transports_45[]={FIPS_PARITY_BLE};
static const fips_parity_step steps_45[]={
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_CONNECTED,.to=0},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(5000)},
    {.kind=FIPS_PARITY_DIAL,.from=0,.to=1,.transport=FIPS_PARITY_BLE},
    {.kind=FIPS_PARITY_ADVANCE,.millis=UINT64_C(25000)},
    {.kind=FIPS_PARITY_HEAL},
};
static const fips_parity_script scripts[]={
    {"crossing-udp-smaller-first-off",settings_0,2,0,transports_0,1,steps_0,7},
    {"crossing-udp-larger-first-off",settings_1,2,1,transports_1,1,steps_1,7},
    {"crossing-udp-smaller-first-on",settings_2,2,0,transports_2,1,steps_2,7},
    {"crossing-udp-larger-first-on",settings_3,2,1,transports_3,1,steps_3,7},
    {"crossing-tcp-smaller-first-off",settings_4,2,0,transports_4,1,steps_4,9},
    {"crossing-tcp-larger-first-off",settings_5,2,1,transports_5,1,steps_5,9},
    {"crossing-tcp-smaller-first-on",settings_6,2,0,transports_6,1,steps_6,9},
    {"crossing-tcp-larger-first-on",settings_7,2,1,transports_7,1,steps_7,9},
    {"crossing-mixed-smaller-off",settings_8,2,0,transports_8,2,steps_8,8},
    {"crossing-mixed-larger-off",settings_9,2,1,transports_9,2,steps_9,8},
    {"crossing-mixed-smaller-on",settings_10,2,0,transports_10,2,steps_10,8},
    {"crossing-mixed-larger-on",settings_11,2,1,transports_11,2,steps_11,8},
    {"crossing-ble-smaller-off",settings_12,2,0,transports_12,1,steps_12,8},
    {"crossing-ble-larger-off",settings_13,2,1,transports_13,1,steps_13,8},
    {"crossing-ble-smaller-on",settings_14,2,0,transports_14,1,steps_14,8},
    {"crossing-ble-larger-on",settings_15,2,1,transports_15,1,steps_15,8},
    {"late-dial-in-order-smaller-off",settings_16,2,0,transports_16,1,steps_16,8},
    {"late-dial-in-order-larger-off",settings_17,2,1,transports_17,1,steps_17,8},
    {"late-dial-in-order-smaller-on",settings_18,2,0,transports_18,1,steps_18,8},
    {"late-dial-in-order-larger-on",settings_19,2,1,transports_19,1,steps_19,8},
    {"late-dial-reordered-smaller-off",settings_20,2,0,transports_20,1,steps_20,9},
    {"late-dial-reordered-larger-off",settings_21,2,1,transports_21,1,steps_21,9},
    {"late-dial-reordered-smaller-on",settings_22,2,0,transports_22,1,steps_22,9},
    {"late-dial-reordered-larger-on",settings_23,2,1,transports_23,1,steps_23,9},
    {"restart-smaller-off",settings_24,2,0,transports_24,1,steps_24,5},
    {"restart-larger-off",settings_25,2,1,transports_25,1,steps_25,5},
    {"restart-smaller-on",settings_26,2,0,transports_26,1,steps_26,5},
    {"restart-larger-on",settings_27,2,1,transports_27,1,steps_27,5},
    {"rekey-own-smaller-off",settings_28,2,0,transports_28,1,steps_28,8},
    {"rekey-own-larger-off",settings_29,2,1,transports_29,1,steps_29,8},
    {"rekey-own-smaller-on",settings_30,2,0,transports_30,1,steps_30,8},
    {"rekey-own-larger-on",settings_31,2,1,transports_31,1,steps_31,8},
    {"rekey-peer-smaller-off",settings_32,2,0,transports_32,1,steps_32,8},
    {"rekey-peer-larger-off",settings_33,2,1,transports_33,1,steps_33,8},
    {"rekey-peer-smaller-on",settings_34,2,0,transports_34,1,steps_34,8},
    {"rekey-peer-larger-on",settings_35,2,1,transports_35,1,steps_35,8},
    {"crossing-tcp-rekey-smaller-off",settings_36,2,0,transports_36,1,steps_36,13},
    {"crossing-tcp-rekey-larger-off",settings_37,2,1,transports_37,1,steps_37,13},
    {"crossing-tcp-rekey-smaller-on",settings_38,2,0,transports_38,1,steps_38,13},
    {"crossing-tcp-rekey-larger-on",settings_39,2,1,transports_39,1,steps_39,13},
    {"redial-transport-smaller-off",settings_40,2,0,transports_40,2,steps_40,9},
    {"redial-transport-larger-off",settings_41,2,1,transports_41,2,steps_41,9},
    {"redial-transport-smaller-on",settings_42,2,0,transports_42,2,steps_42,9},
    {"redial-transport-larger-on",settings_43,2,1,transports_43,2,steps_43,9},
    {"ble-reaped-smaller-on",settings_44,2,0,transports_44,1,steps_44,6},
    {"ble-reaped-larger-on",settings_45,2,1,transports_45,1,steps_45,6},
};
_Static_assert(sizeof(scripts)/sizeof(*scripts)==46,"pinned script count");
static const char *const unshared[][2]={
    {"7, a dial on the peer's aged link, then an own rekey","its dial goes to the peer's second address, and a step dials only a home"},
    {"9, a crossing during a rekey and during its drain","its dials go from and to second addresses, and a step dials only from and to a home"},
    {"10, a stored msg2 whose first send failed","a send that fails, which no step names"},
    {"11, three nodes","a mark holds what an end holds of one peer"},
    {"12, two paths","its second dial goes from a second address, and on joint nodes the link's second path comes from the capability exchange"},
    {"13, a replayed msg1 and a forged msg2 spray","a copy of a captured msg1 and a forged message, which no node sends"},
    {"14, a wall step inside the heard floor","the wall clock stepping apart from the monotonic one"},
    {"16, 17, 20 and 23, a two-path link","the link's second path comes from the capability exchange, started by a feed's candidate, and 20 and 23 add a carrier loss and a transport's removal"},
    {"21, the only path's transport removed","a transport's removal, which no step names"},
    {"22, a NAT rebound of the second path","a NAT's rebinding of a source address, after the capability exchange"},
};
