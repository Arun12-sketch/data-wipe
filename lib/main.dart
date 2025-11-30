// lib/main.dart
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(MaterialApp(
    title: 'CODEWIPE',
    theme: ThemeData.dark(),
    home: MyApp(),
    debugShowCheckedModeBanner: false,
  ));
}

class MyApp extends StatefulWidget {
  @override
  State<StatefulWidget> createState() {
    return Secondappstate();
  }
}

class Secondappstate extends State<MyApp> {
  static const MethodChannel _channel = MethodChannel('com.yourdomain.deviceinfo');

  Color btncolor = Colors.red;
  String SelectedValue = 'Select any one of the following';
  List<String> diskInfoList = [];
  String? selectedDisk;

  bool isWiping = false;
  bool wasWiping = false;   // <-- IMPORTANT (for completion popup)

  int currentPass = 0;
  int currentProgress = 0;
  String wipingStatus = "";

  final supportedMethods = {
    'DoD 5220.22-M Standard': 'dod',
    'NIST SP 800-88 Clear and Purge Guidelines': 'nist',
    'Gutmann Method': 'gutmann',
    'Single Pass Zero or Random Fill': 'singlepass'
  };

  @override
  void initState() {
    super.initState();
    fetchDiskInfo();

    _channel.setMethodCallHandler((call) async {
      if (call.method == 'onWipeProgress') {
        final args = Map<String, dynamic>.from(call.arguments as Map);

        setState(() {
          wasWiping = isWiping; // track state before update

          currentPass = args['pass'] ?? 0;
          currentProgress = args['progress'] ?? 0;
          wipingStatus = args['status'] ?? "";

          if (currentProgress == 100 &&
              (currentPass == 6 || currentPass == 13 || currentPass == 22)) {
            isWiping = false;
            btncolor = Colors.green;
          }

          // Show popup ONLY when going from wiping → not wiping
          if (wasWiping == true && isWiping == false) {
            Future.delayed(Duration(milliseconds: 300), () {
              showDialog(
                context: context,
                builder: (_) => AlertDialog(
                  title: Text("Wipe Completed"),
                  content: Text(
                    "Your disk has been securely wiped and reformatted (NTFS).",
                    style: TextStyle(fontSize: 16),
                  ),
                  actions: [
                    TextButton(
                      child: Text("OK"),
                      onPressed: () => Navigator.pop(context),
                    )
                  ],
                ),
              );
            });
          }
        });
      }
    });
  }

  Future<void> fetchDiskInfo() async {
    try {
      final List<dynamic> disks = await _channel.invokeMethod('getDiskInfo');
      final diskStrings = disks.cast<String>();

      setState(() {
        diskInfoList = diskStrings;
        selectedDisk = diskInfoList.isNotEmpty ? diskInfoList[0] : null;
      });
    } catch (e) {
      setState(() {
        diskInfoList = ['Failed to fetch disk info'];
        selectedDisk = diskInfoList[0];
      });
    }
  }

  // String _extractDeviceFromListLine(String line) {
  //   final token = line.trim().split(RegExp(r'\s+'))[0];
  //   final dev = token.replaceAll(RegExp(r'[^A-Za-z0-9]'), '');
  //   return dev;
  // }
String _extractDeviceFromListLine(String line) {
  for (String part in line.split(" ")) {
    if (part.startsWith("NAME=")) {
      return part.substring(5).replaceAll('"', '');
    }
  }
  return "";
}



  Future<void> startDoD522022MWipe() async {
    if (selectedDisk == null) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Please select a disk')));
      return;
    }

    if (!supportedMethods.containsKey(SelectedValue)) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Select a valid method')));
      return;
    }

    setState(() {
      isWiping = true;
      currentPass = 0;
      currentProgress = 0;
      wipingStatus = "Initializing secure wipe...";
      btncolor = Colors.orange;
    });

    try {
      final dev = _extractDeviceFromListLine(selectedDisk!);
      final devicePath = "/dev/$dev";
      final mode = supportedMethods[SelectedValue]!;

      await _channel.invokeMethod('completelyWipeDisk', {
        'devicePath': devicePath,
        'mode': mode,
      });
    } catch (e) {
      setState(() {
        isWiping = false;
        btncolor = Colors.red;
      });

      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text('Error: $e'), backgroundColor: Colors.red));
    }
  }

  void showalert() {
    showDialog(
        context: context,
        barrierDismissible: false,
        builder: (BuildContext context) {
          return AlertDialog(
              title: Row(
                children: [
                  Icon(Icons.warning, color: Colors.red),
                  SizedBox(width: 10),
                  Text("CRITICAL WARNING!"),
                ],
              ),
              content: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text("You are about to perform a secure wipe:"),
                  SizedBox(height: 10),
                  Text("Method: $SelectedValue",
                      style: TextStyle(fontWeight: FontWeight.bold)),
                  Text("Disk: $selectedDisk",
                      style: TextStyle(fontWeight: FontWeight.bold)),
                  SizedBox(height: 10),
                  Text(
                    "THIS WILL PERMANENTLY DESTROY ALL DATA!\nDisk will be formatted (NTFS) after wiping.",
                    style: TextStyle(color: Colors.red, fontWeight: FontWeight.bold),
                    textAlign: TextAlign.center,
                  ),
                ],
              ),
              actions: [
                TextButton(
                  onPressed: () {
                    Navigator.of(context).pop();
                    btncolor = Colors.red;
                  },
                  child: Text("Cancel"),
                ),
                TextButton(
                  onPressed: () {
                    Navigator.of(context).pop();
                    startDoD522022MWipe();
                  },
                  child: Text("START WIPE", style: TextStyle(color: Colors.red)),
                )
              ]);
        });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        title: Text(
          "CODEWIPE",
          style: TextStyle(color: Colors.white, fontSize: 25, fontWeight: FontWeight.bold),
        ),
        backgroundColor: Colors.black26,
        centerTitle: true,

        // ❌ Removed refresh button completely
      ),

      // appBar: AppBar(
      //   title: Text(
      //     "CODEWIPE",
      //     style: TextStyle(color: Colors.white, fontSize: 25, fontWeight: FontWeight.bold),
      //   ),
      //   backgroundColor: Colors.black26,
      //   centerTitle: true,
      //   actions: [
      //     IconButton(
      //       icon: Icon(Icons.refresh),
      //       onPressed: isWiping ? null : fetchDiskInfo,
      //     )
      //   ],
      // ),
      body: ListView(
        children: [
          Container(
            alignment: Alignment.center,
            decoration: BoxDecoration(
                color: Colors.grey, borderRadius: BorderRadius.circular(26)),
            height: 300,
            margin: EdgeInsets.all(20),
            padding: EdgeInsets.all(16),
            child: SingleChildScrollView(
              child: Text(
                '''                    About CodeWipe

This app securely erases data using DoD 5220.22-M standard, NIST CLEAR, Gutmann (simplified), or Single Pass.
- Passes depend on the chosen method (DoD: 3 passes; NIST: 1 pass + metadata wipe; Gutmann: simplified 4 passes)
- Final step: Create new partition and format (NTFS)
Disk remains usable after wiping.''',
                style: TextStyle(
                    color: Colors.white,
                    fontSize: 15,
                    fontStyle: FontStyle.italic),
              ),
            ),
          ),

          if (isWiping) ...[
            Container(
              margin: EdgeInsets.all(20),
              padding: EdgeInsets.all(16),
              decoration: BoxDecoration(
                color: Colors.orange.withOpacity(0.2),
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: Colors.orange, width: 2),
              ),
              child: Column(
                children: [
                  Text(
                    "Secure Wipe in Progress",
                    style: TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold),
                  ),
                  SizedBox(height: 10),
                  LinearProgressIndicator(
                    value: (currentProgress / 100).clamp(0.0, 1.0),
                    backgroundColor: Colors.grey,
                    valueColor: AlwaysStoppedAnimation(Colors.orange),
                  ),
                  SizedBox(height: 10),
                  Text("Step $currentPass - $currentProgress%",
                      style: TextStyle(color: Colors.white)),
                  Text(wipingStatus,
                      style: TextStyle(color: Colors.white)),
                ],
              ),
            ),
          ],

          Column(
            children: [
              Container(
                alignment: Alignment.center,
                decoration: BoxDecoration(
                    color: Colors.grey,
                    borderRadius: BorderRadius.circular(20),
                    boxShadow: [
                      BoxShadow(
                          spreadRadius: 2,
                          blurRadius: 10,
                          color: Colors.orange,
                          offset: Offset(0, 5))
                    ]),
                padding: EdgeInsets.all(16),
                margin: EdgeInsets.all(20),
                child: DropdownButtonFormField<String>(
                  dropdownColor: Colors.grey,
                  value: SelectedValue,
                  isExpanded: true,
                  decoration: InputDecoration(
                    enabledBorder: InputBorder.none,
                    focusedBorder: InputBorder.none,
                  ),
                  items: <String>[
                    'Select any one of the following',
                    'DoD 5220.22-M Standard',
                    'Gutmann Method',
                    'NIST SP 800-88 Clear and Purge Guidelines',
                    'Single Pass Zero or Random Fill'
                  ].map((String value) {
                    return DropdownMenuItem<String>(
                      value: value,
                      child: Text(
                        value,
                        style: TextStyle(
                            color: Colors.white, fontSize: 15),
                      ),
                    );
                  }).toList(),
                  onChanged: isWiping ? null : (newvalue) {
                    setState(() {
                      SelectedValue = newvalue!;
                    });
                  },
                ),
              ),

              Container(
                alignment: Alignment.center,
                decoration: BoxDecoration(
                    color: Colors.grey,
                    borderRadius: BorderRadius.circular(20),
                    boxShadow: [
                      BoxShadow(
                          spreadRadius: 2,
                          blurRadius: 10,
                          color: Colors.orange,
                          offset: Offset(0, 5))
                    ]),
                padding: EdgeInsets.all(16),
                margin: EdgeInsets.symmetric(horizontal: 20, vertical: 10),
                child: DropdownButtonFormField<String>(
                  dropdownColor: Colors.grey,
                  value: selectedDisk,
                  isExpanded: true,
                  decoration: InputDecoration(
                    enabledBorder: InputBorder.none,
                    focusedBorder: InputBorder.none,
                  ),
                    items: diskInfoList.map((String disk) {
                      String model = "Unknown";
                      String size = "Unknown";
                      String name = "";

                      for (var part in disk.split(" ")) {
                        if (part.startsWith("MODEL=")) model = part.substring(6);
                        if (part.startsWith("SIZE=")) size = part.substring(5);
                        if (part.startsWith("NAME=")) name = part.substring(5);
                      }

                      String display = "$model  $size";

                      return DropdownMenuItem<String>(
                        value: disk,
                        child: Text(
                          display,
                          style: TextStyle(
                            color: Colors.white,
                            fontSize: 15,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                      );
                    }).toList(),



                  // items: diskInfoList.map((String disk) {
                  //   return DropdownMenuItem<String>(
                  //     value: disk,
                  //     child: Text(
                  //       disk,
                  //       style: TextStyle(
                  //           color: Colors.white, fontSize: 15),
                  //     ),
                  //   );
                  // }).toList(),
                  onChanged: isWiping ? null : (newvalue) {
                    setState(() {
                      selectedDisk = newvalue!;
                    });
                  },
                ),
              ),

              Container(
                margin: EdgeInsets.only(top: 20),
                child: MaterialButton(
                  color: btncolor,
                  shape:
                      RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
                  height: 50,
                  hoverColor: isWiping ? null : Colors.orange,
                  padding: EdgeInsets.symmetric(horizontal: 100, vertical: 10),
                  onPressed: isWiping
                      ? null
                      : () {
                          if (!supportedMethods.containsKey(SelectedValue)) {
                            ScaffoldMessenger.of(context).showSnackBar(
                              SnackBar(
                                  content: Text('Please select DoD, NIST, Gutmann or Single Pass')),
                            );
                            return;
                          }
                          showalert();
                        },
                  child: Text(
                    isWiping ? "WIPING..." : "PROCEED",
                    style: TextStyle(color: Colors.white, fontSize: 20),
                  ),
                ),
              )
            ],
          )
        ],
      ),
    );
  }
}
