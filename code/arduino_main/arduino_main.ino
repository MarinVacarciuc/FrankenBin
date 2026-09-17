#include<LiquidCrystal.h>
int  trig = 8;
int echo = 7;
long duration;
int distance;
int ledPin1=6;
int  ledPin2=9;
int ledPin3=12;
int ledPin4=13;
int norisk=A2;
int buzz=A0;
LiquidCrystal  lcd(11, 10, 5, 4, 3, 2);
void setup()
{
pinMode(trig, OUTPUT);
pinMode(echo,  INPUT);
lcd.begin(16, 2);
Serial.begin(9600);
pinMode(ledPin1,OUTPUT);
pinMode(ledPin2,OUTPUT);
pinMode(ledPin3,OUTPUT);
pinMode(ledPin4,OUTPUT);
pinMode(norisk,OUTPUT);
pinMode(buzz,OUTPUT);
lcd.begin(16,2);
lcd.print("Starting  System");
delay(1500);
lcd.clear();
lcd.print("System On");
delay(4000);
}
void loop()
{
digitalWrite(trig,  LOW);
delayMicroseconds(5);
digitalWrite(trig, HIGH);
delayMicroseconds(10);
digitalWrite(trig, LOW);
duration = pulseIn(echo, HIGH);
distance  = duration*0.034/2;
Serial.print("Distance:");
Serial.println(distance);
if (distance <= 25){
lcd.setCursor(0,0);
lcd.print("Extreme  Risk");
lcd.setCursor(0,11);
lcd.print("Glowing 4 LED");
digitalWrite(ledPin1, HIGH);
digitalWrite(ledPin2, HIGH);
digitalWrite(ledPin3,  HIGH);
digitalWrite(ledPin4, HIGH);
digitalWrite(norisk, LOW);
tone(buzz,900);
delay(100);
noTone(buzz);
delay(100);
}
else if (distance >= 26 && distance <=100){
lcd.setCursor(0,0);
lcd.print("High  Risk");
lcd.setCursor(0,11);
lcd.print("Glowing 3 LED");
digitalWrite(ledPin1, HIGH);
digitalWrite(ledPin2, HIGH);
digitalWrite(ledPin3,  HIGH);
digitalWrite(ledPin4, LOW);
digitalWrite(norisk, LOW);
tone(buzz,900);
delay(700);
noTone(buzz);
delay(700);
}
else if (distance >= 101 && distance <=150){
lcd.setCursor(0,0);
lcd.print("Medium  Risk");
lcd.setCursor(0,11);
lcd.print("Glowing 2 LED");
digitalWrite(ledPin1, HIGH);
digitalWrite(ledPin2, HIGH);
digitalWrite(ledPin3, LOW);
digitalWrite(ledPin4, LOW);
digitalWrite(norisk,  LOW);
tone(buzz,1200);
delay(100);
noTone(buzz);
delay(1200);
}
else if (distance >= 151 && distance <=250){
lcd.setCursor(0,0);
lcd.print("Low Risk");
lcd.setCursor(0,11);
lcd.print("Glowing  1 LED");
digitalWrite(ledPin1, HIGH);
digitalWrite(ledPin2,  LOW);
digitalWrite(ledPin3, LOW);
digitalWrite(ledPin4, LOW);
digitalWrite(norisk, LOW);
tone(buzz,900);
delay(300);
noTone(buzz);
delay(2000);
}
else{
lcd.setCursor(0,0);
lcd.print("Safe  No Risk");
lcd.setCursor(0,11);
lcd.print("Glowing Safe LED");
digitalWrite(ledPin1, LOW);
digitalWrite(ledPin2, LOW);
digitalWrite(ledPin3,  LOW);
digitalWrite(ledPin4, LOW);
digitalWrite(norisk, HIGH);
noTone(buzz);
}
}
