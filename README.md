# Intro

wiv - The Web Image Viewer - is an remote image viewer working through the web.

## The story (Or ... scratching my own itch)

I'm often sitting at meetings when my active presence is not needed the entire time. During these times I often connect with my tablet and termux to my local Linux host, where I can continue to developing. One issue that has been bothering me during these times is that there is no easy way that I can transfer an image from my Linux host and have it displayed on my tablet. This was the incentive for the wiv image viewer project. The goal was to be able to issue a command in my Linux shell and have the image displayed on my tablet.

It turns out wiv is quite useful when working from *any* remote computer. 

# The solution

The solution involved the following technologies:

- python3 and asyncio
- The aiohttp web server
- ssh port forwarding
- web socket

Here is a scenario describing how this works:

1. The user adds portforwarding of port 8080 to their ssh configration. This only needs to be done once.
2. The user starts a terminal (or tmux on Android) and connects by ssh to a remote (Linux) host.
3. The user runs the  "wiv-server" command on the remote host.
4. On the local (android) device the open the web page http://localhost:8042. which through portforwarding connects to the wiv server.
5. (On an android device the user my want to use the android split screens support so that tmux and the web browser are be seen side by side.)
6. On the remote server, run `wv /path/to/image.pgm' which sends an image to the wv-server which via websockets forwards it to the web browser.

![wiv tablet screenshot](/wiv-screenshot.jpg "Logo Title Text 1")

# Current state

The project is currently in the prototype phase, but the technology is working. (To test it follows the steps above).

# Todo

- Create a proper python package
- Write better documentation
- Make the server send a prettier web page
- Clean up the old images from the temporary storage on the server
- Support svg files, image overlays, and possibly other formats
