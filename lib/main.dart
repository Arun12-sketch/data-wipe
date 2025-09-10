import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(MaterialApp(
    title: 'CODEWIPE',
    theme: ThemeData.dark(),
    home: MyApp(),
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
  
  // Wiping progress variables
  bool isWiping = false;
  int currentPass = 0;
  int currentProgress = 0;
  String wipingStatus = "";

  @override
  void initState() {
    super.initState();
    fetchDiskInfo();
    
    // Set up method channel listener for progress updates
    _channel.setMethodCallHandler((call) async {
      if (call.method == 'onWipeProgress') {
        final args = call.arguments as Map;
        setState(() {
          currentPass = args['pass'] ?? 0;
          currentProgress = args['progress'] ?? 0;
          wipingStatus = args['status'] ?? "";
          
          // Updated to detect completion after filesystem creation (pass 6)
          if (currentProgress == 100 && (currentPass == 4 || currentPass == 6)) {
            isWiping = false; // Wiping completed
            btncolor = Colors.green;
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
        if (diskInfoList.isNotEmpty) {
          selectedDisk = diskInfoList[0];
        } else {
          selectedDisk = null;
        }
      });
    } on PlatformException catch (e) {
      print('Failed to get disk info: ${e.message}');
      setState(() {
        diskInfoList = ['Failed to fetch disk info'];
        selectedDisk = diskInfoList[0];
      });
    }
  }

  Future<void> startDoD522022MWipe() async {
    if (selectedDisk == null || SelectedValue != 'DoD 5220.22-M Standard') {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Please select DoD 5220.22-M Standard method and a disk')),
      );
      return;
    }

    setState(() {
      isWiping = true;
      currentPass = 0;
      currentProgress = 0;
      wipingStatus = "Starting DoD 5220.22-M wipe + format...";
      btncolor = Colors.orange;
    });

    try {
      // Extract device path from selected disk info
      String devicePath = "/dev/" + selectedDisk!.split(' ')[0];
      
      // CHANGED: Use completelyWipeDisk instead of startDoD522022MWipe
      await _channel.invokeMethod('completelyWipeDisk', {
        'devicePath': devicePath,
      });
    } on PlatformException catch (e) {
      setState(() {
        isWiping = false;
        btncolor = Colors.red;
      });
      
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Error: ${e.message}'), backgroundColor: Colors.red),
      );
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
              Text("You are about to perform DoD 5220.22-M secure wipe:"),
              SizedBox(height: 10),
              Text("Method: $SelectedValue", style: TextStyle(fontWeight: FontWeight.bold)),
              Text("Disk: $selectedDisk", style: TextStyle(fontWeight: FontWeight.bold)),
              SizedBox(height: 10),
              Text(
                "THIS WILL PERMANENTLY DESTROY ALL DATA!\nDisk will be formatted (FAT32) after wiping.",
                style: TextStyle(color: Colors.red, fontWeight: FontWeight.bold),
                textAlign: TextAlign.center,
              ),
            ],
          ),
          actions: [
            MaterialButton(
              color: Colors.grey,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
              onPressed: () {
                Navigator.of(context).pop();
                setState(() {
                  btncolor = Colors.red;
                });
              },
              child: Text("Cancel", style: TextStyle(color: Colors.white, fontSize: 15, fontWeight: FontWeight.bold)),
            ),
            MaterialButton(
              color: Colors.red,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
              hoverColor: Colors.orange,
              onPressed: () {
                Navigator.of(context).pop();
                startDoD522022MWipe(); // THIS IS WHERE THE WIPING STARTS!
              },
              child: Text("START WIPE", style: TextStyle(color: Colors.white, fontSize: 15, fontWeight: FontWeight.bold)),
            )
          ]
        );
      }
    );
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
      ),
      body: ListView(
        children: [
          Container(
            alignment: Alignment.center,
            decoration: BoxDecoration(
                color: Colors.grey, borderRadius: BorderRadius.circular(26)),
            height: 300,
            margin: EdgeInsets.all(20),
            width: double.infinity,
            padding: EdgeInsets.all(16),
            child: SingleChildScrollView(
              child: Text(
                '''                    About CodeWipe
                
This app securely erases data using DoD 5220.22-M standard.
- Pass 1: Overwrite with zeros (0x00)
- Pass 2: Overwrite with ones (0xFF)
- Pass 3: Overwrite with random data
- Step 4: Create new partition and format (FAT32)
Disk remains usable after wiping.''',
                style: TextStyle(
                    color: Colors.white,
                    fontSize: 15,
                    fontStyle: FontStyle.italic),
              ),
            ),
          ),
          
          // Progress indicator when wiping
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
                    "DoD 5220.22-M Wipe in Progress",
                    style: TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold),
                  ),
                  SizedBox(height: 10),
                  LinearProgressIndicator(
                    value: currentProgress / 100.0,
                    backgroundColor: Colors.grey,
                    valueColor: AlwaysStoppedAnimation<Color>(Colors.orange),
                  ),
                  SizedBox(height: 10),
                  Text(
                    "Step $currentPass - $currentProgress%",
                    style: TextStyle(color: Colors.white, fontSize: 16),
                  ),
                  Text(
                    wipingStatus,
                    style: TextStyle(color: Colors.white, fontSize: 14),
                  ),
                ],
              ),
            ),
          ],
          
          Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.center,
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
                    enabledBorder: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(20),
                      borderSide: BorderSide.none,
                    ),
                    focusedBorder: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(20),
                      borderSide: BorderSide.none,
                    ),
                    filled: true,
                    fillColor: Colors.grey,
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
                            color: Colors.white,
                            fontSize: 15,
                            fontWeight: FontWeight.bold),
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
                        offset: Offset(0, 5),
                      )
                    ]),
                padding: EdgeInsets.all(16),
                margin: EdgeInsets.symmetric(horizontal: 20, vertical: 10),
                child: DropdownButtonFormField<String>(
                  dropdownColor: Colors.grey,
                  value: selectedDisk,
                  isExpanded: true,
                  decoration: InputDecoration(
                    enabledBorder: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(20),
                      borderSide: BorderSide.none,
                    ),
                    focusedBorder: OutlineInputBorder(
                      borderRadius: BorderRadius.circular(20),
                      borderSide: BorderSide.none,
                    ),
                    filled: true,
                    fillColor: Colors.grey,
                  ),
                  items: diskInfoList.map((String disk) {
                    return DropdownMenuItem<String>(
                      value: disk,
                      child: Text(
                        disk,
                        style: TextStyle(
                            color: Colors.white,
                            fontSize: 15,
                            fontWeight: FontWeight.bold),
                      ),
                    );
                  }).toList(),
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
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
                  height: 50,
                  hoverColor: isWiping ? null : Colors.orange,
                  padding: EdgeInsets.symmetric(horizontal: 100, vertical: 10),
                  onPressed: isWiping ? null : () {
                    if (SelectedValue != 'DoD 5220.22-M Standard') {
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text('Please select DoD 5220.22-M Standard')),
                      );
                      return;
                    }
                    showalert(); // Show confirmation dialog
                  },
                  child: Text(
                    isWiping ? "WIPING..." : "PROCEED", 
                    style: TextStyle(color: Colors.white, fontSize: 20, fontWeight: FontWeight.bold)
                  ),
                )
              )
            ],
          )
        ],
      ),
    );
  }
}
