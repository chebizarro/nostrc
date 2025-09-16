test_env = environment()

integ = find_program('bash', required: true)
spawn = files('integ/spawn_user_dbus.sh')
add_test('integ:add_account', integ, args: [spawn, files('integ/test_add_account.sh')],
  suite: 'integ',
  timeout: 180)
