{
  'targets': [
    {
      'target_name': 'bluetooth_hci_socket',
      'conditions': [
        ['OS=="linux" or OS=="android" or OS=="freebsd"', {
          'sources': [
            'src/BluetoothHciSocket.cpp'
          ]
        }]
      ],
      "include_dirs" : [
            "<!(node -e \"require('nan')\")"
        ]
    }
  ]
}
