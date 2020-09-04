import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;


public class Case1{
	public static void main(String args[]) throws InterruptedException {

		long count = 0;
		long start = System.currentTimeMillis();

		while(true){
			count++;

			//Pause for 60 seconds
			Thread.sleep(60000);
			//Print a message
			System.out.println(" Sleep cycle count :" + count +" Spleep " + (System.currentTimeMillis()-start)/1000 + "s" );
		}
}


}